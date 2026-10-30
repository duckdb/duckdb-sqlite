#include "sqlite_functions.hpp"

#include <deque>

#include "duckdb/common/crypto/md5.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/interpolate.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/regexp.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "mbedtls_wrapper.hpp"
#include "sqlite_utils.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Values
//===--------------------------------------------------------------------===//
bool SQLiteFunctions::HasRepresentation(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
		return true;
	default:
		return false;
	}
}

bool SQLiteFunctions::TryGetCanonicalText(const Value &value, string &result) {
	date_t date;
	switch (value.type().id()) {
	case LogicalTypeId::DATE:
		date = DateValue::Get(value);
		break;
	case LogicalTypeId::TIMESTAMP: {
		auto ts = TimestampValue::Get(value);
		if (!Value::IsFinite(ts)) {
			return false;
		}
		date = Timestamp::GetDate(ts);
		break;
	}
	default:
		return false;
	}
	if (!Value::IsFinite(date)) {
		return false;
	}
	auto year = Date::ExtractYear(date);
	if (year < 1 || year > 9999) {
		// these are not formatted with four digits - and would not order correctly
		return false;
	}
	result = value.ToString();
	return true;
}

void SQLiteFunctions::SetResult(sqlite3_context *context, const Value &value) {
	if (value.IsNull()) {
		sqlite3_result_null(context);
		return;
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		sqlite3_result_int64(context, BooleanValue::Get(value) ? 1 : 0);
		return;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		sqlite3_result_int64(context, value.GetValue<int64_t>());
		return;
	case LogicalTypeId::DOUBLE: {
		auto result = DoubleValue::Get(value);
		if (std::isnan(result)) {
			throw InvalidInputException("NaN cannot be represented in SQLite - SET sqlite_enable_remote_pushdown = "
			                            "false before attaching the database to execute the query in DuckDB");
		}
		sqlite3_result_double(context, result);
		return;
	}
	case LogicalTypeId::VARCHAR: {
		auto &str = StringValue::Get(value);
		sqlite3_result_text64(context, str.c_str(), str.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
		return;
	}
	case LogicalTypeId::BLOB: {
		auto &str = StringValue::Get(value);
		sqlite3_result_blob64(context, str.c_str(), str.size(), SQLITE_TRANSIENT);
		return;
	}
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP: {
		string text;
		if (!TryGetCanonicalText(value, text)) {
			throw InvalidInputException("The %s value \"%s\" cannot be represented in SQLite - SET "
			                            "sqlite_enable_remote_pushdown = false before attaching the database to "
			                            "execute the query in DuckDB",
			                            value.type().ToString(), value.ToString());
		}
		sqlite3_result_text64(context, text.c_str(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
		return;
	}
	default:
		throw InternalException("SQLiteFunctions::SetResult - unsupported type %s", value.type().ToString());
	}
}

template <class FUNC>
static void SQLiteRunFunction(sqlite3_context *context, FUNC &&func) {
	try {
		func();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		sqlite3_result_error(context, error.Message().c_str(), -1);
	}
}

//! The bytes of a value (the text of numbers)
static string SQLiteValueText(const SQLiteValue &value) {
	switch (value.type) {
	case SQLITE_INTEGER:
		return to_string(value.integer);
	case SQLITE_FLOAT:
		return Value::DOUBLE(value.real).ToString();
	default:
		return string(value.data ? value.data : "", value.size);
	}
}

static string SQLiteTextArgument(sqlite3_value *value) {
	auto data = sqlite3_value_text(value);
	return string(data ? const_char_ptr_cast(data) : "", NumericCast<idx_t>(sqlite3_value_bytes(value)));
}

//! The types of a cast are constant for a statement - they are parsed once and kept as auxiliary data
static const LogicalType &SQLiteTypeArgument(sqlite3_context *context, sqlite3_value **argv, int index) {
	auto cached = static_cast<LogicalType *>(sqlite3_get_auxdata(context, index));
	if (cached) {
		return *cached;
	}
	auto type_id = TransformStringToLogicalTypeId(SQLiteTextArgument(argv[index]));
	auto type = new LogicalType(type_id);
	sqlite3_set_auxdata(context, index, type, [](void *ptr) { delete static_cast<LogicalType *>(ptr); });
	cached = static_cast<LogicalType *>(sqlite3_get_auxdata(context, index));
	if (!cached) {
		throw InternalException("Failed to cache the type argument of a SQLite function");
	}
	return *cached;
}

//===--------------------------------------------------------------------===//
// Functions
//===--------------------------------------------------------------------===//
static void SQLiteErrorFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	auto message = SQLiteTextArgument(argv[0]);
	sqlite3_result_error(context, message.c_str(), -1);
}

//! duckdb_cast(value, source_type, target_type) - a DuckDB cast of a value of the source type
template <bool TRY_CAST>
static void SQLiteCastFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	SQLiteRunFunction(context, [&]() {
		auto &source_type = SQLiteTypeArgument(context, argv, 1);
		auto &target_type = SQLiteTypeArgument(context, argv, 2);
		auto source = SQLiteUtils::ToDuckDBValue(SQLiteUtils::ReadValue(argv[0]), source_type);
		if (TRY_CAST) {
			auto result = source.DefaultTryCastAs(target_type);
			SQLiteFunctions::SetResult(context, result ? *result : Value(target_type));
		} else {
			SQLiteFunctions::SetResult(context, source.DefaultCastAs(target_type));
		}
	});
}

//! duckdb_date(value) / duckdb_timestamp(value) - a value of a DATE / TIMESTAMP column as DuckDB reads it
template <LogicalTypeId TYPE>
static void SQLiteReadFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	SQLiteRunFunction(context, [&]() {
		SQLiteFunctions::SetResult(context,
		                           SQLiteUtils::ToDuckDBValue(SQLiteUtils::ReadValue(argv[0]), LogicalType(TYPE)));
	});
}

template <bool IS_UPPER>
static void SQLiteCaseConvertFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto input = SQLiteTextArgument(argv[0]);
		string result;
		result.reserve(input.size());
		for (idx_t i = 0; i < input.size();) {
			if (!(input[i] & 0x80)) {
				result += char(IS_UPPER ? StringUtil::ASCII_TO_UPPER_MAP[uint8_t(input[i])]
				                        : StringUtil::ASCII_TO_LOWER_MAP[uint8_t(input[i])]);
				i++;
				continue;
			}
			int size = 0;
			auto codepoint = Utf8Proc::UTF8ToCodepoint(input.c_str() + i, size, input.size() - i);
			if (size <= 0) {
				throw InvalidInputException("Invalid UTF-8 in string");
			}
			auto converted = IS_UPPER ? Utf8Proc::CodepointToUpper(codepoint) : Utf8Proc::CodepointToLower(codepoint);
			char buffer[4];
			int new_size = 0;
			if (!Utf8Proc::CodepointToUtf8(converted, new_size, buffer)) {
				throw InvalidInputException("Invalid UTF-8 in string");
			}
			result.append(buffer, NumericCast<idx_t>(new_size));
			i += NumericCast<idx_t>(size);
		}
		sqlite3_result_text64(context, result.c_str(), result.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
	});
}

//! DuckDB's LIKE - '_' matches a single (UTF-8) character, there is no escape character unless one is given
static bool SQLiteLikeMatches(const char *sdata, idx_t slen, const char *pdata, idx_t plen, bool has_escape,
                              char escape) {
	idx_t pidx = 0;
	idx_t sidx = 0;
	bool has_percentage = false;
	idx_t retry_pidx = 0;
	idx_t retry_sidx = 0;
	while (sidx < slen) {
		if (pidx < plen) {
			char pchar = pdata[pidx];
			char schar = sdata[sidx];
			if (has_escape && pchar == escape) {
				if (pidx + 1 == plen) {
					throw SyntaxException("Like pattern must not end with escape character!");
				}
				if (pdata[pidx + 1] == schar) {
					pidx += 2;
					sidx++;
					continue;
				}
			} else if (pchar == '_') {
				pidx++;
				sidx++;
				while (sidx < slen && (sdata[sidx] & 0xc0) == 0x80) {
					sidx++;
				}
				continue;
			} else if (pchar == '%') {
				pidx++;
				while (pidx < plen && pdata[pidx] == '%') {
					pidx++;
				}
				if (pidx == plen) {
					return true;
				}
				has_percentage = true;
				retry_pidx = pidx;
				retry_sidx = sidx;
				continue;
			} else if (pchar == schar) {
				pidx++;
				sidx++;
				continue;
			}
		}
		if (!has_percentage) {
			return false;
		}
		pidx = retry_pidx;
		sidx = ++retry_sidx;
	}
	while (pidx < plen) {
		if (has_escape && pdata[pidx] == escape) {
			if (pidx + 1 == plen) {
				throw SyntaxException("Like pattern must not end with escape character!");
			}
			break;
		}
		if (pdata[pidx] != '%') {
			break;
		}
		pidx++;
	}
	return pidx == plen && sidx == slen;
}

static string SQLiteLowerCase(const string &input) {
	auto length = LowerLength(input.c_str(), input.size());
	string result(length, '\0');
	LowerCase(input.c_str(), input.size(), &result[0]);
	return result;
}

//! duckdb_like(string, pattern[, escape]) / duckdb_ilike(string, pattern[, escape])
template <bool CASE_INSENSITIVE>
static void SQLiteLikeFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	for (int i = 0; i < argc; i++) {
		if (sqlite3_value_type(argv[i]) == SQLITE_NULL) {
			sqlite3_result_null(context);
			return;
		}
	}
	SQLiteRunFunction(context, [&]() {
		auto str = SQLiteTextArgument(argv[0]);
		auto pattern = SQLiteTextArgument(argv[1]);
		bool has_escape = false;
		char escape = '\0';
		if (argc > 2) {
			auto escape_str = SQLiteTextArgument(argv[2]);
			if (escape_str.size() > 1) {
				throw SyntaxException("Invalid escape string. Escape string must be empty or one character.");
			}
			has_escape = !escape_str.empty();
			escape = has_escape ? escape_str[0] : '\0';
		}
		if (CASE_INSENSITIVE) {
			str = SQLiteLowerCase(str);
			pattern = SQLiteLowerCase(pattern);
			escape = StringUtil::CharacterToLower(escape);
		}
		bool result = SQLiteLikeMatches(str.c_str(), str.size(), pattern.c_str(), pattern.size(), has_escape, escape);
		sqlite3_result_int64(context, result ? 1 : 0);
	});
}

static void SQLiteGlobFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL) {
		sqlite3_result_null(context);
		return;
	}
	auto str = SQLiteTextArgument(argv[0]);
	auto pattern = SQLiteTextArgument(argv[1]);
	sqlite3_result_int64(context, Glob(str.c_str(), str.size(), pattern.c_str(), pattern.size()) ? 1 : 0);
}

//===--------------------------------------------------------------------===//
// Math
//===--------------------------------------------------------------------===//
static bool SQLiteHasNull(int argc, sqlite3_value **argv) {
	for (int i = 0; i < argc; i++) {
		if (sqlite3_value_type(argv[i]) == SQLITE_NULL) {
			return true;
		}
	}
	return false;
}

static double SQLiteDoubleArgument(sqlite3_value *value) {
	if (sqlite3_value_type(value) == SQLITE_INTEGER) {
		return double(sqlite3_value_int64(value));
	}
	return sqlite3_value_double(value);
}

static void SQLiteSetDouble(sqlite3_context *context, double value) {
	SQLiteFunctions::SetResult(context, Value::DOUBLE(value));
}

typedef double (*sqlite_unary_math_t)(double);

//! duckdb_<function>(x) - a DOUBLE function (with IEEE semantics, like DuckDB by default)
static void SQLiteUnaryMathFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	auto function = reinterpret_cast<sqlite_unary_math_t>(sqlite3_user_data(context));
	SQLiteRunFunction(context, [&]() { SQLiteSetDouble(context, function(SQLiteDoubleArgument(argv[0]))); });
}

static constexpr double SQLITE_PI = 3.141592653589793;

static double SQLiteSqrt(double x) {
	return std::sqrt(x);
}
static double SQLiteCbrt(double x) {
	return std::cbrt(x);
}
static double SQLiteLn(double x) {
	return std::log(x);
}
static double SQLiteLog10(double x) {
	return std::log10(x);
}
static double SQLiteLog2(double x) {
	return std::log2(x);
}
static double SQLiteExp(double x) {
	return std::exp(x);
}
static double SQLiteSin(double x) {
	return std::sin(x);
}
static double SQLiteCos(double x) {
	return std::cos(x);
}
static double SQLiteTan(double x) {
	return std::tan(x);
}
static double SQLiteCot(double x) {
	return 1.0 / std::tan(x);
}
static double SQLiteAsin(double x) {
	return std::asin(x);
}
static double SQLiteAcos(double x) {
	return std::acos(x);
}
static double SQLiteAtan(double x) {
	return std::atan(x);
}
static double SQLiteSinh(double x) {
	return std::sinh(x);
}
static double SQLiteCosh(double x) {
	return std::cosh(x);
}
static double SQLiteTanh(double x) {
	return std::tanh(x);
}
static double SQLiteAsinh(double x) {
	return std::asinh(x);
}
static double SQLiteAcosh(double x) {
	return std::acosh(x);
}
static double SQLiteAtanh(double x) {
	return std::atanh(x);
}
static double SQLiteDegrees(double x) {
	return x * (180 / SQLITE_PI);
}
static double SQLiteRadians(double x) {
	return x * (SQLITE_PI / 180);
}
static double SQLiteCeil(double x) {
	return std::ceil(x);
}
static double SQLiteFloor(double x) {
	return std::floor(x);
}
static double SQLiteTrunc(double x) {
	return std::trunc(x);
}
static double SQLiteEven(double x) {
	double value = x >= 0 ? std::ceil(x) : -std::ceil(-x);
	if (std::floor(value / 2) * 2 != value) {
		return x >= 0 ? value + 1 : value - 1;
	}
	return value;
}

struct SQLiteUnaryMathDefinition {
	const char *name;
	sqlite_unary_math_t function;
};

static const SQLiteUnaryMathDefinition SQLITE_UNARY_MATH_FUNCTIONS[] = {
    {"duckdb_sqrt", SQLiteSqrt},   {"duckdb_cbrt", SQLiteCbrt},       {"duckdb_ln", SQLiteLn},
    {"duckdb_log10", SQLiteLog10}, {"duckdb_log2", SQLiteLog2},       {"duckdb_exp", SQLiteExp},
    {"duckdb_sin", SQLiteSin},     {"duckdb_cos", SQLiteCos},         {"duckdb_tan", SQLiteTan},
    {"duckdb_cot", SQLiteCot},     {"duckdb_asin", SQLiteAsin},       {"duckdb_acos", SQLiteAcos},
    {"duckdb_atan", SQLiteAtan},   {"duckdb_sinh", SQLiteSinh},       {"duckdb_cosh", SQLiteCosh},
    {"duckdb_tanh", SQLiteTanh},   {"duckdb_asinh", SQLiteAsinh},     {"duckdb_acosh", SQLiteAcosh},
    {"duckdb_atanh", SQLiteAtanh}, {"duckdb_degrees", SQLiteDegrees}, {"duckdb_radians", SQLiteRadians},
    {"duckdb_ceil", SQLiteCeil},   {"duckdb_floor", SQLiteFloor},     {"duckdb_trunc", SQLiteTrunc},
    {"duckdb_even", SQLiteEven},
};

static void SQLitePowFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		SQLiteSetDouble(context, std::pow(SQLiteDoubleArgument(argv[0]), SQLiteDoubleArgument(argv[1])));
	});
}

static void SQLiteAtan2Function(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		SQLiteSetDouble(context, std::atan2(SQLiteDoubleArgument(argv[0]), SQLiteDoubleArgument(argv[1])));
	});
}

//! duckdb_shift_left(x, shift, max_shift) - max_shift is the number of value bits of the type (plus one for unsigned
//! types)
static void SQLiteShiftLeftFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		int64_t input = sqlite3_value_int64(argv[0]);
		int64_t shift = sqlite3_value_int64(argv[1]);
		int64_t max_shift = sqlite3_value_int64(argv[2]);
		if (input < 0) {
			throw OutOfRangeException("Cannot left-shift negative number %s", NumericHelper::ToString(input));
		}
		if (shift < 0) {
			throw OutOfRangeException("Cannot left-shift by negative number %s", NumericHelper::ToString(shift));
		}
		if (shift >= max_shift) {
			if (input == 0) {
				sqlite3_result_int64(context, 0);
				return;
			}
			throw OutOfRangeException("Left-shift value %s is out of range", NumericHelper::ToString(shift));
		}
		if (shift == 0 || input == 0) {
			sqlite3_result_int64(context, input);
			return;
		}
		auto max_value = int64_t(1) << (max_shift - shift - 1);
		if (input >= max_value) {
			throw OutOfRangeException("Overflow in left shift (%s << %s)", NumericHelper::ToString(input),
			                          NumericHelper::ToString(shift));
		}
		sqlite3_result_int64(context, input << shift);
	});
}

//! duckdb_shift_right(x, shift, bits)
static void SQLiteShiftRightFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	int64_t input = sqlite3_value_int64(argv[0]);
	int64_t shift = sqlite3_value_int64(argv[1]);
	int64_t bits = sqlite3_value_int64(argv[2]);
	sqlite3_result_int64(context, shift >= 0 && shift < bits ? input >> shift : 0);
}

//! duckdb_check_type(value, type, column) - raises the error the table scan raises for a value whose storage class
//! does not match the type of its column (BIGINT, DOUBLE or VARCHAR)
static void SQLiteCheckTypeFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	SQLiteRunFunction(context, [&]() {
		auto storage_type = sqlite3_value_type(argv[0]);
		auto type = SQLiteTextArgument(argv[1]);
		int expected_type;
		bool matches;
		if (type == "BIGINT") {
			expected_type = SQLITE_INTEGER;
			matches = storage_type == SQLITE_INTEGER;
		} else if (type == "DOUBLE") {
			expected_type = SQLITE_FLOAT;
			matches = storage_type == SQLITE_FLOAT || storage_type == SQLITE_INTEGER;
		} else {
			expected_type = SQLITE_TEXT;
			matches = storage_type == SQLITE_TEXT;
		}
		if (!matches && storage_type != SQLITE_NULL) {
			auto text = sqlite3_value_text(argv[0]);
			auto expected = type == "DOUBLE" ? string("expected float or integer")
			                                 : "column was declared as " + SQLiteUtils::TypeToString(expected_type);
			throw Exception(ExceptionType::MISMATCH_TYPE,
			                "Invalid type in column \"" + SQLiteTextArgument(argv[2]) + "\": " + expected +
			                    ", found \"" + string(text ? const_char_ptr_cast(text) : "") + "\" of type \"" +
			                    SQLiteUtils::TypeToString(storage_type) + "\" instead.");
		}
		sqlite3_result_int(context, 1);
	});
}

//! duckdb_double(bits) - the double with the given bits (SQLite does not read every double literal exactly)
static void SQLiteDoubleFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	int64_t bits = sqlite3_value_int64(argv[0]);
	double value;
	memcpy(&value, &bits, sizeof(value));
	sqlite3_result_double(context, value);
}

//! duckdb_divide(x, y) - a floating point division (e.g. by a signed zero)
static void SQLiteDivideFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(
	    context, [&]() { SQLiteSetDouble(context, SQLiteDoubleArgument(argv[0]) / SQLiteDoubleArgument(argv[1])); });
}

//! duckdb_log(base, x)
static void SQLiteLogBaseFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		SQLiteSetDouble(context, std::log10(SQLiteDoubleArgument(argv[1])) / std::log10(SQLiteDoubleArgument(argv[0])));
	});
}

//! duckdb_round(x, precision, half_even) / duckdb_trunc_precision(x, precision) for doubles
template <bool TRUNCATE>
static void SQLiteRoundFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto input = SQLiteDoubleArgument(argv[0]);
		int64_t precision = sqlite3_value_int64(argv[1]);
		bool half_even = !TRUNCATE && argc > 2 && sqlite3_value_int64(argv[2]) != 0;
		auto nearest = [&](double value) {
			if (TRUNCATE) {
				return std::trunc(value);
			}
			return half_even ? std::nearbyint(value) : std::round(value);
		};
		double result;
		if (precision < 0) {
			double modifier = std::pow(10, -double(precision));
			if (std::isinf(modifier)) {
				// every finite value rounds to zero
				result = Value::IsFinite(input) ? 0 : input;
			} else {
				result = nearest(input / modifier) * modifier;
			}
		} else {
			double modifier = std::pow(10, double(precision));
			result = nearest(input * modifier) / modifier;
		}
		if (std::isinf(result) || std::isnan(result)) {
			// the input is too large to be rounded to this precision
			result = input;
		}
		SQLiteSetDouble(context, result);
	});
}

//! duckdb_round_integer(x, precision, half_even) - rounding of integers to a negative precision
static void SQLiteRoundIntegerFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		int64_t input = sqlite3_value_int64(argv[0]);
		int64_t precision = sqlite3_value_int64(argv[1]);
		bool half_even = sqlite3_value_int64(argv[2]) != 0;
		if (precision >= 0) {
			sqlite3_result_int64(context, input);
			return;
		}
		if (precision <= -Hugeint::CACHED_POWERS_OF_TEN) {
			sqlite3_result_int64(context, 0);
			return;
		}
		auto power_of_ten = Hugeint::POWERS_OF_TEN[-precision];
		auto half = power_of_ten / 2;
		hugeint_t wide_input(input);
		auto rounded = wide_input / power_of_ten;
		auto remainder = wide_input % power_of_ten;
		auto rounds_away = [&](const hugeint_t &value) {
			return !half_even || value % hugeint_t(2) != hugeint_t(0);
		};
		if (remainder > half || (remainder == half && rounds_away(rounded))) {
			rounded = Hugeint::Add(rounded, 1);
		} else if (remainder < -half || (remainder == -half && rounds_away(rounded))) {
			rounded = Hugeint::Subtract(rounded, 1);
		}
		hugeint_t result;
		int64_t result_value;
		if (!Hugeint::TryMultiply(rounded, power_of_ten, result) || !Hugeint::TryCast(result, result_value)) {
			throw OutOfRangeException("Overflow in %s of integer", half_even ? "ROUND_EVEN" : "ROUND");
		}
		sqlite3_result_int64(context, result_value);
	});
}

//===--------------------------------------------------------------------===//
// Dates and times
//===--------------------------------------------------------------------===//
//! Dates and timestamps are represented as their text in pushed down queries
static timestamp_t SQLiteTimestampArgument(sqlite3_value *value) {
	auto text = SQLiteTextArgument(value);
	return Timestamp::FromCString(text.c_str(), text.size());
}

static int64_t SQLiteFloorDivide(int64_t value, int64_t units) {
	return (value + (value < 0)) / units - (value < 0);
}

//! duckdb_date_part(part, value)
static void SQLiteDatePartFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto part = StringUtil::Lower(SQLiteTextArgument(argv[0]));
		auto ts = SQLiteTimestampArgument(argv[1]);
		auto date = Timestamp::GetDate(ts);
		auto time = Timestamp::GetTime(ts);
		int64_t result;
		if (part == "year") {
			result = Date::ExtractYear(date);
		} else if (part == "month") {
			result = Date::ExtractMonth(date);
		} else if (part == "day") {
			result = Date::ExtractDay(date);
		} else if (part == "decade") {
			result = Date::ExtractYear(date) / 10;
		} else if (part == "century") {
			auto year = Date::ExtractYear(date);
			result = year > 0 ? ((year - 1) / 100) + 1 : (year / 100) - 1;
		} else if (part == "millennium") {
			auto year = Date::ExtractYear(date);
			result = year > 0 ? ((year - 1) / 1000) + 1 : (year / 1000) - 1;
		} else if (part == "quarter") {
			result = (Date::ExtractMonth(date) - 1) / Interval::MONTHS_PER_QUARTER + 1;
		} else if (part == "dow") {
			result = Date::ExtractISODayOfTheWeek(date) % 7;
		} else if (part == "isodow") {
			result = Date::ExtractISODayOfTheWeek(date);
		} else if (part == "doy") {
			result = Date::ExtractDayOfTheYear(date);
		} else if (part == "week") {
			result = Date::ExtractISOWeekNumber(date);
		} else if (part == "isoyear") {
			result = Date::ExtractISOYearNumber(date);
		} else if (part == "yearweek") {
			int32_t yyyy, ww;
			Date::ExtractISOYearWeek(date, yyyy, ww);
			result = int64_t(yyyy) * 100 + (yyyy > 0 ? ww : -ww);
		} else if (part == "era") {
			result = Date::ExtractYear(date) > 0 ? 1 : 0;
		} else if (part == "microsecond") {
			result = time.value % Interval::MICROS_PER_MINUTE;
		} else if (part == "millisecond") {
			result = (time.value % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_MSEC;
		} else if (part == "second") {
			result = (time.value % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_SEC;
		} else if (part == "minute") {
			result = (time.value % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE;
		} else if (part == "hour") {
			result = time.value / Interval::MICROS_PER_HOUR;
		} else if (part == "epoch") {
			SQLiteSetDouble(context, double(Timestamp::GetEpochMicroSeconds(ts)) / double(Interval::MICROS_PER_SEC));
			return;
		} else if (part == "julian") {
			SQLiteSetDouble(context, Timestamp::GetJulianDay(ts));
			return;
		} else if (part == "epoch_ms") {
			result = Timestamp::GetEpochMs(ts);
		} else if (part == "epoch_us") {
			result = Timestamp::GetEpochMicroSeconds(ts);
		} else if (part == "epoch_ns") {
			result = Timestamp::GetEpochNanoSeconds(ts);
		} else if (part == "dayname" || part == "monthname") {
			auto name = part == "dayname" ? Date::DAY_NAMES[Date::ExtractISODayOfTheWeek(date) % 7]
			                              : Date::MONTH_NAMES[Date::ExtractMonth(date) - 1];
			sqlite3_result_text64(context, name.GetData(), name.GetSize(), SQLITE_TRANSIENT, SQLITE_UTF8);
			return;
		} else {
			throw InvalidInputException("Unsupported date part \"%s\"", part);
		}
		sqlite3_result_int64(context, result);
	});
}

static timestamp_t SQLiteTruncFixed(timestamp_t ts, int64_t interval_us) {
	const int64_t v = ts.value;
	const int64_t q = v / interval_us - (v % interval_us != 0 && v < 0 ? 1 : 0);
	return timestamp_t(q * interval_us);
}

//! duckdb_date_trunc(part, value) - the result is a TIMESTAMP
static void SQLiteDateTruncFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto part = StringUtil::Lower(SQLiteTextArgument(argv[0]));
		auto ts = SQLiteTimestampArgument(argv[1]);
		auto date = Timestamp::GetDate(ts);
		auto year = Date::ExtractYear(date);
		date_t result_date;
		if (part == "millennium") {
			result_date = Date::FromDate((year / 1000) * 1000, 1, 1);
		} else if (part == "century") {
			result_date = Date::FromDate((year / 100) * 100, 1, 1);
		} else if (part == "decade") {
			result_date = Date::FromDate((year / 10) * 10, 1, 1);
		} else if (part == "year") {
			result_date = Date::FromDate(year, 1, 1);
		} else if (part == "quarter") {
			auto month = Date::ExtractMonth(date);
			result_date = Date::FromDate(year, 1 + (((month - 1) / 3) * 3), 1);
		} else if (part == "month") {
			result_date = Date::FromDate(year, Date::ExtractMonth(date), 1);
		} else if (part == "week") {
			result_date = Date::GetMondayOfCurrentWeek(date);
		} else if (part == "isoyear") {
			auto monday = Date::GetMondayOfCurrentWeek(date);
			result_date =
			    Date::SubtractDays(monday, (Date::ExtractISOWeekNumber(monday) - 1) * Interval::DAYS_PER_WEEK);
		} else if (part == "day") {
			result_date = date;
		} else {
			timestamp_t result;
			if (part == "hour") {
				result = SQLiteTruncFixed(ts, Interval::MICROS_PER_HOUR);
			} else if (part == "minute") {
				result = SQLiteTruncFixed(ts, Interval::MICROS_PER_MINUTE);
			} else if (part == "second") {
				result = SQLiteTruncFixed(ts, Interval::MICROS_PER_SEC);
			} else if (part == "millisecond") {
				result = SQLiteTruncFixed(ts, Interval::MICROS_PER_MSEC);
			} else if (part == "microsecond") {
				result = ts;
			} else {
				throw InvalidInputException("Unsupported date part \"%s\"", part);
			}
			SQLiteFunctions::SetResult(context, Value::TIMESTAMP(result));
			return;
		}
		SQLiteFunctions::SetResult(context, Value::TIMESTAMP(Timestamp::FromDatetime(result_date, dtime_t(0))));
	});
}

//! duckdb_date_diff(part, start, end) - the number of part boundaries between the values
static void SQLiteDateDiffFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto part = StringUtil::Lower(SQLiteTextArgument(argv[0]));
		auto start = SQLiteTimestampArgument(argv[1]);
		auto end = SQLiteTimestampArgument(argv[2]);
		auto start_date = Timestamp::GetDate(start);
		auto end_date = Timestamp::GetDate(end);
		auto diff = [](int64_t s, int64_t e, int64_t units) {
			return SQLiteFloorDivide(e, units) - SQLiteFloorDivide(s, units);
		};
		auto months = [](date_t date) {
			int32_t year, month, day;
			Date::Convert(date, year, month, day);
			return int64_t(year) * 12 + month - 1;
		};
		int64_t result;
		if (part == "year") {
			result = Date::ExtractYear(end_date) - Date::ExtractYear(start_date);
		} else if (part == "month") {
			result = months(end_date) - months(start_date);
		} else if (part == "day") {
			result = int64_t(Date::EpochDays(end_date)) - int64_t(Date::EpochDays(start_date));
		} else if (part == "decade") {
			result = Date::ExtractYear(end_date) / 10 - Date::ExtractYear(start_date) / 10;
		} else if (part == "century") {
			result = Date::ExtractYear(end_date) / 100 - Date::ExtractYear(start_date) / 100;
		} else if (part == "millennium") {
			result = Date::ExtractYear(end_date) / 1000 - Date::ExtractYear(start_date) / 1000;
		} else if (part == "quarter") {
			result =
			    months(end_date) / Interval::MONTHS_PER_QUARTER - months(start_date) / Interval::MONTHS_PER_QUARTER;
		} else if (part == "week") {
			// weeks do not count Monday crossings, just distance
			result = (int64_t(end_date.days) - int64_t(start_date.days)) / Interval::DAYS_PER_WEEK;
		} else if (part == "isoyear") {
			result = Date::ExtractISOYearNumber(end_date) - Date::ExtractISOYearNumber(start_date);
		} else if (part == "microsecond") {
			if (!TrySubtractOperator::Operation(Timestamp::GetEpochMicroSeconds(end),
			                                    Timestamp::GetEpochMicroSeconds(start), result)) {
				throw OutOfRangeException("Overflow in subtraction of INT64");
			}
		} else if (part == "millisecond") {
			result = diff(start.value, end.value, Interval::MICROS_PER_MSEC);
		} else if (part == "second") {
			result = diff(start.value, end.value, Interval::MICROS_PER_SEC);
		} else if (part == "minute") {
			result = diff(start.value, end.value, Interval::MICROS_PER_MINUTE);
		} else if (part == "hour") {
			result = diff(start.value, end.value, Interval::MICROS_PER_HOUR);
		} else {
			throw InvalidInputException("Unsupported date part \"%s\"", part);
		}
		sqlite3_result_int64(context, result);
	});
}

//! duckdb_add_interval(value, months, days, micros) - the result is a TIMESTAMP
static void SQLiteAddIntervalFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto ts = SQLiteTimestampArgument(argv[0]);
		interval_t interval;
		interval.months = NumericCast<int32_t>(sqlite3_value_int64(argv[1]));
		interval.days = NumericCast<int32_t>(sqlite3_value_int64(argv[2]));
		interval.micros = sqlite3_value_int64(argv[3]);
		SQLiteFunctions::SetResult(context, Value::TIMESTAMP(Interval::Add(ts, interval)));
	});
}

//! duckdb_strftime(value, format)
static void SQLiteStrftimeFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto result = StrfTimeFormat::Format(SQLiteTimestampArgument(argv[0]), SQLiteTextArgument(argv[1]));
		sqlite3_result_text64(context, result.c_str(), result.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
	});
}

//! duckdb_make_date(year, month, day)
static void SQLiteMakeDateFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto year = NumericCast<int32_t>(sqlite3_value_int64(argv[0]));
		auto month = NumericCast<int32_t>(sqlite3_value_int64(argv[1]));
		auto day = NumericCast<int32_t>(sqlite3_value_int64(argv[2]));
		date_t result;
		if (!Date::TryFromDate(year, month, day, result)) {
			throw ConversionException("Date out of range: %d-%d-%d", year, month, day);
		}
		SQLiteFunctions::SetResult(context, Value::DATE(result));
	});
}

//===--------------------------------------------------------------------===//
// Strings
//===--------------------------------------------------------------------===//
//! The regular expressions of a statement are constant (in most cases) - they are compiled once and kept as auxiliary
//! data
static duckdb_re2::RE2 &SQLiteRegexArgument(sqlite3_context *context, sqlite3_value **argv, int pattern_index,
                                            int options_index, bool *global_replace,
                                            bool *no_match_returns_input = nullptr) {
	duckdb_re2::RE2::Options options;
	options.set_log_errors(false);
	auto options_str = SQLiteTextArgument(argv[options_index]);
	if (!options_str.empty()) {
		regexp_util::ParseRegexOptions(options_str, options, global_replace, no_match_returns_input);
	}
	auto cached = static_cast<duckdb_re2::RE2 *>(sqlite3_get_auxdata(context, pattern_index));
	if (cached) {
		return *cached;
	}
	auto pattern = SQLiteTextArgument(argv[pattern_index]);
	auto re = new duckdb_re2::RE2(duckdb_re2::StringPiece(pattern), options);
	if (!re->ok()) {
		auto error = re->error();
		delete re;
		throw InvalidInputException(error);
	}
	sqlite3_set_auxdata(context, pattern_index, re, [](void *ptr) { delete static_cast<duckdb_re2::RE2 *>(ptr); });
	cached = static_cast<duckdb_re2::RE2 *>(sqlite3_get_auxdata(context, pattern_index));
	if (!cached) {
		throw InternalException("Failed to cache a regular expression");
	}
	return *cached;
}

//! duckdb_regexp_matches(string, pattern, options) / duckdb_regexp_full_match(string, pattern, options)
template <bool FULL_MATCH>
static void SQLiteRegexpMatchesFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto &re = SQLiteRegexArgument(context, argv, 1, 2, nullptr);
		auto str = SQLiteTextArgument(argv[0]);
		duckdb_re2::StringPiece input(str);
		bool result = FULL_MATCH ? duckdb_re2::RE2::FullMatch(input, re) : duckdb_re2::RE2::PartialMatch(input, re);
		sqlite3_result_int64(context, result ? 1 : 0);
	});
}

//! duckdb_regexp_replace(string, pattern, replacement, options)
static void SQLiteRegexpReplaceFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		bool global_replace = false;
		auto &re = SQLiteRegexArgument(context, argv, 1, 3, &global_replace);
		auto replace = SQLiteTextArgument(argv[2]);
		duckdb_re2::StringPiece replace_piece(replace);
		std::string rewrite_error;
		if (!re.CheckRewriteString(replace_piece, &rewrite_error)) {
			throw InvalidInputException("Invalid replacement string for regexp_replace: %s", rewrite_error);
		}
		auto str = SQLiteTextArgument(argv[0]);
		if (global_replace) {
			duckdb_re2::RE2::GlobalReplace(&str, re, replace_piece);
		} else {
			duckdb_re2::RE2::Replace(&str, re, replace_piece);
		}
		regexp_util::VerifyUTF8Result(str.c_str(), str.size());
		sqlite3_result_text64(context, str.c_str(), str.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
	});
}

//! duckdb_regexp_extract(string, pattern, group, options)
static void SQLiteRegexpExtractFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		bool no_match_returns_input = false;
		auto &re = SQLiteRegexArgument(context, argv, 1, 3, nullptr, &no_match_returns_input);
		auto str = SQLiteTextArgument(argv[0]);
		int64_t group_index = sqlite3_value_int64(argv[2]);
		if (group_index < 0 || group_index > re.NumberOfCapturingGroups()) {
			auto result = no_match_returns_input ? str : string();
			sqlite3_result_text64(context, result.c_str(), result.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
			return;
		}
		duckdb_re2::StringPiece input(str);
		auto nsubmatch = NumericCast<int>(group_index + 1);
		vector<duckdb_re2::StringPiece> submatch(NumericCast<idx_t>(nsubmatch));
		string result;
		if (re.Match(input, 0, input.size(), duckdb_re2::RE2::UNANCHORED, submatch.data(), nsubmatch)) {
			auto &group = submatch[group_index];
			regexp_util::VerifyUTF8Result(group.data(), group.size());
			result = string(group.data(), group.size());
		} else if (no_match_returns_input) {
			result = str;
		}
		sqlite3_result_text64(context, result.c_str(), result.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
	});
}

//! duckdb_to_base64(blob) / duckdb_from_base64(string)
template <bool TO_BASE64>
static void SQLiteBase64Function(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto data = SQLiteValueText(SQLiteUtils::ReadValue(argv[0]));
		string_t input(data.c_str(), NumericCast<uint32_t>(data.size()));
		if (TO_BASE64) {
			auto result = Blob::ToBase64(input);
			sqlite3_result_text64(context, result.c_str(), result.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
		} else {
			auto result = Blob::FromBase64(input);
			sqlite3_result_blob64(context, result.c_str(), result.size(), SQLITE_TRANSIENT);
		}
	});
}

static void SQLiteSetText(sqlite3_context *context, const char *data, idx_t size) {
	sqlite3_result_text64(context, data, size, SQLITE_TRANSIENT, SQLITE_UTF8);
}

static int64_t SQLiteCharacterCount(const string &str) {
	int64_t count = 0;
	for (auto c : str) {
		count += IsCharacter(c);
	}
	return count;
}

//! The substring of a string with DuckDB's semantics (offsets / lengths that are negative or out of range)
static string SQLiteSubstring(const string &str, int64_t offset, int64_t length) {
	Vector result(LogicalType::VARCHAR, 1);
	auto substring = SubstringUnicode(result, string_t(str.c_str(), NumericCast<uint32_t>(str.size())), offset, length);
	return substring.GetString();
}

//! duckdb_substring(string, offset[, length])
static void SQLiteSubstringFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto length = argc > 2 ? sqlite3_value_int64(argv[2]) : int64_t(NumericLimits<uint32_t>::Maximum());
		auto result = SQLiteSubstring(SQLiteTextArgument(argv[0]), sqlite3_value_int64(argv[1]), length);
		SQLiteSetText(context, result.c_str(), result.size());
	});
}

//! duckdb_left(string, count) / duckdb_right(string, count) - negative counts remove characters
template <bool LEFT>
static void SQLiteLeftRightFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto str = SQLiteTextArgument(argv[0]);
		int64_t pos = sqlite3_value_int64(argv[1]);
		auto num_characters = SQLiteCharacterCount(str);
		string result;
		if (LEFT) {
			if (pos < 0) {
				pos = MaxValue<int64_t>(0, num_characters + pos);
			}
			result = SQLiteSubstring(str, 1, pos);
		} else {
			int64_t len;
			if (pos >= 0) {
				len = MinValue<int64_t>(num_characters, pos);
			} else {
				len = pos == NumericLimits<int64_t>::Minimum()
				          ? 0
				          : num_characters - MinValue<int64_t>(num_characters, -pos);
			}
			result = SQLiteSubstring(str, num_characters - len + 1, len);
		}
		SQLiteSetText(context, result.c_str(), result.size());
	});
}

//! duckdb_lpad(string, count, padding) / duckdb_rpad(string, count, padding)
template <bool LEFT>
static void SQLitePadFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto str = SQLiteTextArgument(argv[0]);
		auto len = NumericCast<idx_t>(MaxValue<int64_t>(0, NumericCast<int32_t>(sqlite3_value_int64(argv[1]))));
		auto pad = SQLiteTextArgument(argv[2]);
		// how much of the string fits into the result
		idx_t str_bytes = 0;
		idx_t str_chars = 0;
		for (; str_chars < len && str_bytes < str.size(); ++str_chars) {
			int32_t codepoint;
			str_bytes += DecodeCodepoint(str.c_str() + str_bytes, str.size() - str_bytes, codepoint);
		}
		string padding;
		auto missing = len - str_chars;
		if (missing > 0 && pad.empty()) {
			throw InvalidInputException(LEFT ? "Insufficient padding in LPAD." : "Insufficient padding in RPAD.");
		}
		idx_t pad_bytes = 0;
		for (idx_t i = 0; i < missing; i++) {
			if (pad_bytes >= pad.size()) {
				padding += pad;
				pad_bytes = 0;
			}
			int32_t codepoint;
			pad_bytes += DecodeCodepoint(pad.c_str() + pad_bytes, pad.size() - pad_bytes, codepoint);
		}
		padding += pad.substr(0, pad_bytes);
		auto result = LEFT ? padding + str.substr(0, str_bytes) : str.substr(0, str_bytes) + padding;
		SQLiteSetText(context, result.c_str(), result.size());
	});
}

//! duckdb_reverse(string) - reverses the grapheme clusters
static void SQLiteReverseFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto str = SQLiteTextArgument(argv[0]);
		string result(str.size(), '\0');
		for (auto cluster : Utf8Proc::GraphemeClusters(str.c_str(), str.size())) {
			memcpy(&result[0] + str.size() - cluster.end, str.c_str() + cluster.start, cluster.end - cluster.start);
		}
		SQLiteSetText(context, result.c_str(), result.size());
	});
}

//! duckdb_md5(value) / duckdb_sha256(value)
template <bool SHA256>
static void SQLiteHashFunction(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (SQLiteHasNull(argc, argv)) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto data = SQLiteValueText(SQLiteUtils::ReadValue(argv[0]));
		string result;
		if (SHA256) {
			duckdb_mbedtls::MbedTlsWrapper::SHA256State state;
			state.AddString(data);
			auto hash = state.Finalize();
			static const char *HEX_DIGITS = "0123456789abcdef";
			for (auto c : hash) {
				auto byte = static_cast<uint8_t>(c);
				result += HEX_DIGITS[byte >> 4];
				result += HEX_DIGITS[byte & 0x0F];
			}
		} else {
			MD5Context md5;
			md5.Add(data);
			result = md5.FinishHex();
		}
		sqlite3_result_text64(context, result.c_str(), result.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
	});
}

//===--------------------------------------------------------------------===//
// Aggregates
//===--------------------------------------------------------------------===//
//! Exceptions must not escape into SQLite - the callbacks of the aggregates are wrapped with these
template <void (*STEP)(sqlite3_context *, int, sqlite3_value **)>
static void SQLiteSafeStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	try {
		STEP(context, argc, argv);
	} catch (std::bad_alloc &) {
		sqlite3_result_error_nomem(context);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		sqlite3_result_error(context, error.Message().c_str(), -1);
	}
}

template <void (*FINAL)(sqlite3_context *)>
static void SQLiteSafeFinal(sqlite3_context *context) {
	try {
		FINAL(context);
	} catch (std::bad_alloc &) {
		sqlite3_result_error_nomem(context);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		sqlite3_result_error(context, error.Message().c_str(), -1);
	}
}

//! Adds a copy of a value to the values an aggregate keeps - sqlite3_value_dup returns NULL when it runs out of memory
static void SQLiteAppendValue(vector<sqlite3_value *> &values, sqlite3_value *value) {
	auto copy = sqlite3_value_dup(value);
	if (!copy) {
		throw std::bad_alloc();
	}
	try {
		values.push_back(copy);
	} catch (...) {
		sqlite3_value_free(copy);
		throw;
	}
}

static sqlite3_value *SQLiteDupValue(sqlite3_value *value) {
	auto result = sqlite3_value_dup(value);
	if (!result) {
		throw std::bad_alloc();
	}
	return result;
}

//! The value of a scalar subquery - DuckDB raises an error when the subquery returns more than one row
struct SQLiteScalarSubqueryState {
	bool has_value;
	sqlite3_value *value;
};

static void SQLiteScalarSubqueryStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	auto state =
	    static_cast<SQLiteScalarSubqueryState *>(sqlite3_aggregate_context(context, sizeof(SQLiteScalarSubqueryState)));
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	if (state->has_value) {
		sqlite3_result_error(context,
		                     "More than one row returned by a subquery used as an expression - scalar "
		                     "subqueries can only return a single row.",
		                     -1);
		return;
	}
	state->value = SQLiteDupValue(argv[0]);
	state->has_value = true;
}

static void SQLiteScalarSubqueryFinal(sqlite3_context *context) {
	auto state = static_cast<SQLiteScalarSubqueryState *>(sqlite3_aggregate_context(context, 0));
	if (!state || !state->has_value) {
		sqlite3_result_null(context);
		return;
	}
	sqlite3_result_value(context, state->value);
	sqlite3_value_free(state->value);
	state->value = nullptr;
}

//! The variance / standard deviation of the values - with the algorithm of DuckDB
struct SQLiteVarianceState {
	uint64_t count;
	double mean;
	double dsquared;
};

static void SQLiteVarianceStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		return;
	}
	auto state = static_cast<SQLiteVarianceState *>(sqlite3_aggregate_context(context, sizeof(SQLiteVarianceState)));
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	auto input = SQLiteDoubleArgument(argv[0]);
	state->count++;
	const double mean_differential = (input - state->mean) / double(state->count);
	const double new_mean = state->mean + mean_differential;
	const double dsquared_increment = (input - new_mean) * (input - state->mean);
	state->mean = new_mean;
	state->dsquared = state->dsquared + dsquared_increment;
}

enum class SQLiteVarianceKind { VAR_SAMP, VAR_POP, STDDEV_SAMP, STDDEV_POP };

template <SQLiteVarianceKind KIND>
static void SQLiteVarianceFinal(sqlite3_context *context) {
	auto state = static_cast<SQLiteVarianceState *>(sqlite3_aggregate_context(context, 0));
	bool sample = KIND == SQLiteVarianceKind::VAR_SAMP || KIND == SQLiteVarianceKind::STDDEV_SAMP;
	if (!state || state->count == 0 || (sample && state->count <= 1)) {
		sqlite3_result_null(context);
		return;
	}
	double result;
	if (sample) {
		result = state->dsquared / double(state->count - 1);
	} else {
		result = state->count > 1 ? state->dsquared / double(state->count) : 0;
	}
	if (KIND == SQLiteVarianceKind::STDDEV_SAMP || KIND == SQLiteVarianceKind::STDDEV_POP) {
		result = std::sqrt(result);
	}
	SQLiteRunFunction(context, [&]() { SQLiteSetDouble(context, result); });
}

struct SQLiteProductState {
	bool has_value;
	double product;
};

static void SQLiteProductStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		return;
	}
	auto state = static_cast<SQLiteProductState *>(sqlite3_aggregate_context(context, sizeof(SQLiteProductState)));
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	auto input = SQLiteDoubleArgument(argv[0]);
	state->product = state->has_value ? state->product * input : input;
	state->has_value = true;
}

static void SQLiteProductFinal(sqlite3_context *context) {
	auto state = static_cast<SQLiteProductState *>(sqlite3_aggregate_context(context, 0));
	if (!state || !state->has_value) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() { SQLiteSetDouble(context, state->product); });
}

//! duckdb_avg(x) - the integers are summed exactly (SQLite sums them as floating point numbers once the sum overflows)
struct SQLiteAvgState {
	idx_t count;
	hugeint_t integer_sum;
	double double_sum;
	bool has_double;
};

template <bool INVERSE>
static void SQLiteAvgStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	auto type = sqlite3_value_type(argv[0]);
	if (type == SQLITE_NULL) {
		return;
	}
	auto state = static_cast<SQLiteAvgState *>(sqlite3_aggregate_context(context, sizeof(SQLiteAvgState)));
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	if (type == SQLITE_INTEGER) {
		hugeint_t value(sqlite3_value_int64(argv[0]));
		if (INVERSE) {
			state->integer_sum -= value;
		} else {
			state->integer_sum += value;
		}
	} else {
		auto value = SQLiteDoubleArgument(argv[0]);
		state->double_sum += INVERSE ? -value : value;
		state->has_double = true;
	}
	if (INVERSE) {
		state->count--;
	} else {
		state->count++;
	}
}

static void SQLiteAvgValue(sqlite3_context *context) {
	auto state = static_cast<SQLiteAvgState *>(sqlite3_aggregate_context(context, 0));
	if (!state || state->count == 0) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		if (!state->has_double) {
			auto sum = Hugeint::Cast<long double>(state->integer_sum);
			SQLiteSetDouble(context, double(sum / (long double)state->count));
			return;
		}
		auto sum = state->double_sum + Hugeint::Cast<double>(state->integer_sum);
		SQLiteSetDouble(context, sum / double(state->count));
	});
}

//! duckdb_frame_sum(x) / duckdb_frame_avg(x) - window functions that sum the values of the frame from scratch,
//! instead of subtracting the values that leave the frame from the sum (which can lose all floating point precision)
struct SQLiteFrameState {
	std::deque<double> values;
};

static SQLiteFrameState *SQLiteGetFrameState(sqlite3_context *context, bool create) {
	auto state =
	    static_cast<SQLiteFrameState **>(sqlite3_aggregate_context(context, create ? sizeof(SQLiteFrameState *) : 0));
	if (!state) {
		return nullptr;
	}
	if (!*state && create) {
		*state = new SQLiteFrameState();
	}
	return *state;
}

template <bool INVERSE>
static void SQLiteFrameStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		return;
	}
	SQLiteRunFunction(context, [&]() {
		auto state = SQLiteGetFrameState(context, true);
		if (!state) {
			throw std::bad_alloc();
		}
		if (INVERSE) {
			// the values leave the frame in the order they entered it
			state->values.pop_front();
		} else {
			state->values.push_back(SQLiteDoubleArgument(argv[0]));
		}
	});
}

template <bool AVG>
static void SQLiteFrameValue(sqlite3_context *context) {
	auto state = SQLiteGetFrameState(context, false);
	if (!state || state->values.empty()) {
		sqlite3_result_null(context);
		return;
	}
	SQLiteRunFunction(context, [&]() {
		double sum = 0;
		for (auto value : state->values) {
			sum += value;
		}
		SQLiteSetDouble(context, AVG ? sum / double(state->values.size()) : sum);
	});
}

template <bool AVG>
static void SQLiteFrameFinal(sqlite3_context *context) {
	SQLiteFrameValue<AVG>(context);
	auto state = SQLiteGetFrameState(context, false);
	delete state;
}

enum class SQLiteBitKind { AND, OR, XOR };

struct SQLiteBitState {
	bool has_value;
	int64_t value;
};

template <SQLiteBitKind KIND>
static void SQLiteBitStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		return;
	}
	auto state = static_cast<SQLiteBitState *>(sqlite3_aggregate_context(context, sizeof(SQLiteBitState)));
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	int64_t input = sqlite3_value_int64(argv[0]);
	if (!state->has_value) {
		state->value = input;
		state->has_value = true;
		return;
	}
	switch (KIND) {
	case SQLiteBitKind::AND:
		state->value &= input;
		break;
	case SQLiteBitKind::OR:
		state->value |= input;
		break;
	case SQLiteBitKind::XOR:
		state->value ^= input;
		break;
	}
}

static void SQLiteBitFinal(sqlite3_context *context) {
	auto state = static_cast<SQLiteBitState *>(sqlite3_aggregate_context(context, 0));
	if (!state || !state->has_value) {
		sqlite3_result_null(context);
		return;
	}
	sqlite3_result_int64(context, state->value);
}

//! Compare two (non-NULL) values of the same type the way DuckDB compares them - numbers by their value, strings and
//! blobs byte by byte
static int SQLiteCompareValues(sqlite3_value *left, sqlite3_value *right) {
	auto left_type = sqlite3_value_type(left);
	auto right_type = sqlite3_value_type(right);
	bool left_numeric = left_type == SQLITE_INTEGER || left_type == SQLITE_FLOAT;
	bool right_numeric = right_type == SQLITE_INTEGER || right_type == SQLITE_FLOAT;
	if (left_numeric && right_numeric) {
		if (left_type == SQLITE_INTEGER && right_type == SQLITE_INTEGER) {
			int64_t l = sqlite3_value_int64(left);
			int64_t r = sqlite3_value_int64(right);
			return l < r ? -1 : (l > r ? 1 : 0);
		}
		auto l = sqlite3_value_double(left);
		auto r = sqlite3_value_double(right);
		return l < r ? -1 : (l > r ? 1 : 0);
	}
	if (left_numeric != right_numeric) {
		// like SQLite orders values of different storage classes
		return left_numeric ? -1 : 1;
	}
	auto l = left_type == SQLITE_BLOB ? sqlite3_value_blob(left) : sqlite3_value_text(left);
	auto r = right_type == SQLITE_BLOB ? sqlite3_value_blob(right) : sqlite3_value_text(right);
	auto l_size = idx_t(sqlite3_value_bytes(left));
	auto r_size = idx_t(sqlite3_value_bytes(right));
	auto result = memcmp(l, r, MinValue(l_size, r_size));
	if (result != 0) {
		return result;
	}
	return l_size < r_size ? -1 : (l_size > r_size ? 1 : 0);
}

//! The state of an aggregate that keeps values - the aggregate context only holds a pointer to it
template <class T>
struct SQLitePointerState {
	T *data;
};

template <class T>
static T *SQLiteGetPointerState(sqlite3_context *context, bool create) {
	auto state = static_cast<SQLitePointerState<T> *>(
	    sqlite3_aggregate_context(context, create ? sizeof(SQLitePointerState<T>) : 0));
	if (!state) {
		return nullptr;
	}
	if (!state->data && create) {
		state->data = new T();
	}
	return state->data;
}

template <class T>
static void SQLiteFreePointerState(sqlite3_context *context) {
	auto state = static_cast<SQLitePointerState<T> *>(sqlite3_aggregate_context(context, 0));
	if (state && state->data) {
		delete state->data;
		state->data = nullptr;
	}
}

//! Frees the state of an aggregate when its final function returns (or throws)
template <class T>
struct SQLiteFreePointerStateGuard {
	explicit SQLiteFreePointerStateGuard(sqlite3_context *context_p) : context(context_p) {
	}
	~SQLiteFreePointerStateGuard() {
		SQLiteFreePointerState<T>(context);
	}
	sqlite3_context *context;
};

//! arg_min / arg_max - the argument of the row with the minimum / maximum value, rows where either is NULL are ignored
struct SQLiteArgState {
	~SQLiteArgState() {
		if (arg) {
			sqlite3_value_free(arg);
		}
		if (val) {
			sqlite3_value_free(val);
		}
	}
	sqlite3_value *arg = nullptr;
	sqlite3_value *val = nullptr;
};

template <bool IS_MAX>
static void SQLiteArgStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL) {
		return;
	}
	auto state = SQLiteGetPointerState<SQLiteArgState>(context, true);
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	if (state->val) {
		auto comparison = SQLiteCompareValues(argv[1], state->val);
		if (IS_MAX ? comparison <= 0 : comparison >= 0) {
			return;
		}
		sqlite3_value_free(state->arg);
		sqlite3_value_free(state->val);
	}
	state->arg = nullptr;
	state->val = nullptr;
	state->arg = SQLiteDupValue(argv[0]);
	state->val = SQLiteDupValue(argv[1]);
}

static void SQLiteArgFinal(sqlite3_context *context) {
	SQLiteFreePointerStateGuard<SQLiteArgState> guard(context);
	auto state = SQLiteGetPointerState<SQLiteArgState>(context, false);
	if (!state || !state->arg) {
		sqlite3_result_null(context);
	} else {
		sqlite3_result_value(context, state->arg);
	}
}

//! quantile_cont / quantile_disc - with the interpolation of DuckDB
struct SQLiteQuantileState {
	~SQLiteQuantileState() {
		for (auto value : values) {
			sqlite3_value_free(value);
		}
	}
	vector<sqlite3_value *> values;
	double quantile = 0;
	//! A DECIMAL quantile (given as its text) - DuckDB computes the index of a discrete quantile exactly then
	bool is_decimal = false;
	hugeint_t integral;
	hugeint_t scaling;
};

//! Parses a decimal quantile like "0.25" into its integral value and scaling (25 and 100)
static bool SQLiteParseDecimalQuantile(const string &text, hugeint_t &integral, hugeint_t &scaling) {
	integral = hugeint_t(0);
	scaling = hugeint_t(1);
	bool fraction = false;
	idx_t digits = 0;
	for (auto c : text) {
		if (c == '.' && !fraction) {
			fraction = true;
			continue;
		}
		if (c < '0' || c > '9' || digits >= 30) {
			return false;
		}
		integral = integral * hugeint_t(10) + hugeint_t(c - '0');
		if (fraction) {
			scaling = scaling * hugeint_t(10);
		}
		digits++;
	}
	return digits > 0 && integral <= scaling;
}

static void SQLiteQuantileStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		return;
	}
	auto state = SQLiteGetPointerState<SQLiteQuantileState>(context, true);
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	if (sqlite3_value_type(argv[1]) == SQLITE_TEXT) {
		state->is_decimal = true;
		if (!SQLiteParseDecimalQuantile(SQLiteTextArgument(argv[1]), state->integral, state->scaling)) {
			throw InvalidInputException("The quantile must be between 0 and 1");
		}
	} else {
		auto quantile = SQLiteDoubleArgument(argv[1]);
		if (sqlite3_value_type(argv[1]) == SQLITE_NULL || !(quantile >= 0 && quantile <= 1)) {
			throw InvalidInputException("The quantile must be between 0 and 1");
		}
		state->quantile = quantile;
	}
	SQLiteAppendValue(state->values, argv[0]);
}

template <bool DISCRETE>
static void SQLiteQuantileFinal(sqlite3_context *context) {
	SQLiteFreePointerStateGuard<SQLiteQuantileState> guard(context);
	auto state = SQLiteGetPointerState<SQLiteQuantileState>(context, false);
	if (!state || state->values.empty()) {
		sqlite3_result_null(context);
		return;
	}
	auto &values = state->values;
	auto n = values.size();
	auto comparator = [](sqlite3_value *left, sqlite3_value *right) {
		return SQLiteCompareValues(left, right) < 0;
	};
	std::sort(values.begin(), values.end(), comparator);
	if (DISCRETE) {
		idx_t floored;
		if (state->is_decimal) {
			// integer arithmetic, like DuckDB
			auto wide_n = hugeint_t(NumericCast<int64_t>(n));
			int64_t result;
			Hugeint::TryCast((wide_n * state->scaling - wide_n * state->integral) / state->scaling, result);
			floored = NumericCast<idx_t>(result);
		} else {
			// a separate statement like in DuckDB - n - n * q in one expression can be contracted to a fused
			// multiply-subtract, which rounds differently
			const auto scaled_q = double(n) * state->quantile;
			floored = idx_t(std::floor(double(n) - scaled_q));
		}
		auto index = MaxValue<idx_t>(1, n - floored) - 1;
		sqlite3_result_value(context, values[index]);
	} else {
		auto quantile = state->is_decimal
		                    ? Hugeint::Cast<double>(state->integral) / Hugeint::Cast<double>(state->scaling)
		                    : state->quantile;
		double rn = double(n - 1) * quantile;
		auto frn = idx_t(std::floor(rn));
		auto crn = idx_t(std::ceil(rn));
		auto lo = SQLiteDoubleArgument(values[frn]);
		double result = lo;
		if (frn != crn) {
			result = InterpolateOperator::Operation<double>(lo, rn - double(frn), SQLiteDoubleArgument(values[crn]));
		}
		SQLiteSetDouble(context, result);
	}
}

//! duckdb_string_agg(value, separator, order, key...) - string_agg with an ORDER BY, the order has two characters per
//! key: A (ascending) / D (descending) and F (NULLs first) / L (NULLs last)
struct SQLiteOrderedStringAggState {
	struct Row {
		string value;
		vector<sqlite3_value *> keys;
	};
	~SQLiteOrderedStringAggState() {
		for (auto &row : rows) {
			for (auto key : row.keys) {
				sqlite3_value_free(key);
			}
		}
	}
	vector<Row> rows;
	string separator;
	string order;
};

static void SQLiteOrderedStringAggStep(sqlite3_context *context, int argc, sqlite3_value **argv) {
	if (argc < 3) {
		sqlite3_result_error(context, "duckdb_string_agg requires a value, a separator and an order", -1);
		return;
	}
	if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
		// NULL values are skipped
		return;
	}
	auto state = SQLiteGetPointerState<SQLiteOrderedStringAggState>(context, true);
	if (!state) {
		sqlite3_result_error_nomem(context);
		return;
	}
	state->separator = SQLiteTextArgument(argv[1]);
	state->order = SQLiteTextArgument(argv[2]);
	// the row is added before its keys are copied - the state frees them if a copy fails
	state->rows.emplace_back();
	auto &row = state->rows.back();
	row.value = SQLiteTextArgument(argv[0]);
	for (int i = 3; i < argc; i++) {
		SQLiteAppendValue(row.keys, argv[i]);
	}
}

static void SQLiteOrderedStringAggFinal(sqlite3_context *context) {
	SQLiteFreePointerStateGuard<SQLiteOrderedStringAggState> guard(context);
	auto state = SQLiteGetPointerState<SQLiteOrderedStringAggState>(context, false);
	if (!state || state->rows.empty()) {
		sqlite3_result_null(context);
		return;
	}
	auto &order = state->order;
	auto comparator = [&](const SQLiteOrderedStringAggState::Row &left, const SQLiteOrderedStringAggState::Row &right) {
		for (idx_t k = 0; k < left.keys.size(); k++) {
			bool descending = k * 2 < order.size() && order[k * 2] == 'D';
			bool nulls_first = k * 2 + 1 < order.size() && order[k * 2 + 1] == 'F';
			bool left_null = sqlite3_value_type(left.keys[k]) == SQLITE_NULL;
			bool right_null = sqlite3_value_type(right.keys[k]) == SQLITE_NULL;
			if (left_null || right_null) {
				if (left_null && right_null) {
					continue;
				}
				return left_null ? nulls_first : !nulls_first;
			}
			auto comparison = SQLiteCompareValues(left.keys[k], right.keys[k]);
			if (comparison != 0) {
				return descending ? comparison > 0 : comparison < 0;
			}
		}
		return false;
	};
	std::stable_sort(state->rows.begin(), state->rows.end(), comparator);
	string result;
	for (idx_t i = 0; i < state->rows.size(); i++) {
		if (i > 0) {
			result += state->separator;
		}
		result += state->rows[i].value;
	}
	SQLiteSetText(context, result.c_str(), result.size());
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
void SQLiteFunctions::Register(sqlite3 *db) {
	struct ScalarFunctionDefinition {
		const char *name;
		int argc;
		void (*function)(sqlite3_context *, int, sqlite3_value **);
	};
	static const ScalarFunctionDefinition SCALAR_FUNCTIONS[] = {
	    {"duckdb_cast", 3, SQLiteCastFunction<false>},
	    {"duckdb_try_cast", 3, SQLiteCastFunction<true>},
	    {"duckdb_date", 1, SQLiteReadFunction<LogicalTypeId::DATE>},
	    {"duckdb_timestamp", 1, SQLiteReadFunction<LogicalTypeId::TIMESTAMP>},
	    {"duckdb_lower", 1, SQLiteCaseConvertFunction<false>},
	    {"duckdb_upper", 1, SQLiteCaseConvertFunction<true>},
	    {"duckdb_like", 2, SQLiteLikeFunction<false>},
	    {"duckdb_like", 3, SQLiteLikeFunction<false>},
	    {"duckdb_ilike", 2, SQLiteLikeFunction<true>},
	    {"duckdb_ilike", 3, SQLiteLikeFunction<true>},
	    {"duckdb_glob", 2, SQLiteGlobFunction},
	    {"duckdb_pow", 2, SQLitePowFunction},
	    {"duckdb_atan2", 2, SQLiteAtan2Function},
	    {"duckdb_divide", 2, SQLiteDivideFunction},
	    {"duckdb_double", 1, SQLiteDoubleFunction},
	    {"duckdb_check_type", 3, SQLiteCheckTypeFunction},
	    {"duckdb_shift_left", 3, SQLiteShiftLeftFunction},
	    {"duckdb_shift_right", 3, SQLiteShiftRightFunction},
	    {"duckdb_log", 2, SQLiteLogBaseFunction},
	    {"duckdb_round", 3, SQLiteRoundFunction<false>},
	    {"duckdb_trunc_precision", 2, SQLiteRoundFunction<true>},
	    {"duckdb_round_integer", 3, SQLiteRoundIntegerFunction},
	    {"duckdb_date_part", 2, SQLiteDatePartFunction},
	    {"duckdb_date_trunc", 2, SQLiteDateTruncFunction},
	    {"duckdb_date_diff", 3, SQLiteDateDiffFunction},
	    {"duckdb_add_interval", 4, SQLiteAddIntervalFunction},
	    {"duckdb_strftime", 2, SQLiteStrftimeFunction},
	    {"duckdb_make_date", 3, SQLiteMakeDateFunction},
	    {"duckdb_regexp_matches", 3, SQLiteRegexpMatchesFunction<false>},
	    {"duckdb_regexp_full_match", 3, SQLiteRegexpMatchesFunction<true>},
	    {"duckdb_regexp_replace", 4, SQLiteRegexpReplaceFunction},
	    {"duckdb_regexp_extract", 4, SQLiteRegexpExtractFunction},
	    {"duckdb_to_base64", 1, SQLiteBase64Function<true>},
	    {"duckdb_from_base64", 1, SQLiteBase64Function<false>},
	    {"duckdb_substring", 2, SQLiteSubstringFunction},
	    {"duckdb_substring", 3, SQLiteSubstringFunction},
	    {"duckdb_left", 2, SQLiteLeftRightFunction<true>},
	    {"duckdb_right", 2, SQLiteLeftRightFunction<false>},
	    {"duckdb_lpad", 3, SQLitePadFunction<true>},
	    {"duckdb_rpad", 3, SQLitePadFunction<false>},
	    {"duckdb_reverse", 1, SQLiteReverseFunction},
	    {"duckdb_md5", 1, SQLiteHashFunction<false>},
	    {"duckdb_sha256", 1, SQLiteHashFunction<true>},
	};
	// the functions can only be used by top-level SQL - not by the views, triggers, ... stored in a database file
	int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY;
	for (auto &function : SCALAR_FUNCTIONS) {
		SQLiteUtils::Check(sqlite3_create_function_v2(db, function.name, function.argc, flags, nullptr,
		                                              function.function, nullptr, nullptr, nullptr),
		                   db);
	}
	for (auto &function : SQLITE_UNARY_MATH_FUNCTIONS) {
		SQLiteUtils::Check(sqlite3_create_function_v2(db, function.name, 1, flags,
		                                              reinterpret_cast<void *>(function.function),
		                                              SQLiteUnaryMathFunction, nullptr, nullptr, nullptr),
		                   db);
	}
	// not deterministic - so that SQLite does not evaluate it once for a constant argument
	SQLiteUtils::Check(sqlite3_create_function_v2(db, "duckdb_error", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, nullptr,
	                                              SQLiteErrorFunction, nullptr, nullptr, nullptr),
	                   db);
	struct AggregateFunctionDefinition {
		const char *name;
		int argc;
		void (*step)(sqlite3_context *, int, sqlite3_value **);
		void (*final)(sqlite3_context *);
	};
	static const AggregateFunctionDefinition AGGREGATE_FUNCTIONS[] = {
	    {"duckdb_scalar_subquery", 1, SQLiteSafeStep<SQLiteScalarSubqueryStep>,
	     SQLiteSafeFinal<SQLiteScalarSubqueryFinal>},
	    {"duckdb_var_samp", 1, SQLiteSafeStep<SQLiteVarianceStep>,
	     SQLiteSafeFinal<SQLiteVarianceFinal<SQLiteVarianceKind::VAR_SAMP>>},
	    {"duckdb_var_pop", 1, SQLiteSafeStep<SQLiteVarianceStep>,
	     SQLiteSafeFinal<SQLiteVarianceFinal<SQLiteVarianceKind::VAR_POP>>},
	    {"duckdb_stddev_samp", 1, SQLiteSafeStep<SQLiteVarianceStep>,
	     SQLiteSafeFinal<SQLiteVarianceFinal<SQLiteVarianceKind::STDDEV_SAMP>>},
	    {"duckdb_stddev_pop", 1, SQLiteSafeStep<SQLiteVarianceStep>,
	     SQLiteSafeFinal<SQLiteVarianceFinal<SQLiteVarianceKind::STDDEV_POP>>},
	    {"duckdb_product", 1, SQLiteSafeStep<SQLiteProductStep>, SQLiteSafeFinal<SQLiteProductFinal>},
	    {"duckdb_bit_and", 1, SQLiteSafeStep<SQLiteBitStep<SQLiteBitKind::AND>>, SQLiteSafeFinal<SQLiteBitFinal>},
	    {"duckdb_bit_or", 1, SQLiteSafeStep<SQLiteBitStep<SQLiteBitKind::OR>>, SQLiteSafeFinal<SQLiteBitFinal>},
	    {"duckdb_bit_xor", 1, SQLiteSafeStep<SQLiteBitStep<SQLiteBitKind::XOR>>, SQLiteSafeFinal<SQLiteBitFinal>},
	    {"duckdb_arg_min", 2, SQLiteSafeStep<SQLiteArgStep<false>>, SQLiteSafeFinal<SQLiteArgFinal>},
	    {"duckdb_arg_max", 2, SQLiteSafeStep<SQLiteArgStep<true>>, SQLiteSafeFinal<SQLiteArgFinal>},
	    {"duckdb_quantile_cont", 2, SQLiteSafeStep<SQLiteQuantileStep>, SQLiteSafeFinal<SQLiteQuantileFinal<false>>},
	    {"duckdb_quantile_disc", 2, SQLiteSafeStep<SQLiteQuantileStep>, SQLiteSafeFinal<SQLiteQuantileFinal<true>>},
	    {"duckdb_string_agg", -1, SQLiteSafeStep<SQLiteOrderedStringAggStep>,
	     SQLiteSafeFinal<SQLiteOrderedStringAggFinal>},
	};
	for (auto &function : AGGREGATE_FUNCTIONS) {
		SQLiteUtils::Check(sqlite3_create_function_v2(db, function.name, function.argc, flags, nullptr, nullptr,
		                                              function.step, function.final, nullptr),
		                   db);
	}
	struct WindowFunctionDefinition {
		const char *name;
		void (*step)(sqlite3_context *, int, sqlite3_value **);
		void (*final)(sqlite3_context *);
		void (*value)(sqlite3_context *);
		void (*inverse)(sqlite3_context *, int, sqlite3_value **);
	};
	static const WindowFunctionDefinition WINDOW_FUNCTIONS[] = {
	    {"duckdb_avg", SQLiteSafeStep<SQLiteAvgStep<false>>, SQLiteSafeFinal<SQLiteAvgValue>,
	     SQLiteSafeFinal<SQLiteAvgValue>, SQLiteSafeStep<SQLiteAvgStep<true>>},
	    {"duckdb_frame_sum", SQLiteSafeStep<SQLiteFrameStep<false>>, SQLiteSafeFinal<SQLiteFrameFinal<false>>,
	     SQLiteSafeFinal<SQLiteFrameValue<false>>, SQLiteSafeStep<SQLiteFrameStep<true>>},
	    {"duckdb_frame_avg", SQLiteSafeStep<SQLiteFrameStep<false>>, SQLiteSafeFinal<SQLiteFrameFinal<true>>,
	     SQLiteSafeFinal<SQLiteFrameValue<true>>, SQLiteSafeStep<SQLiteFrameStep<true>>},
	};
	for (auto &function : WINDOW_FUNCTIONS) {
		SQLiteUtils::Check(sqlite3_create_window_function(db, function.name, 1, flags, nullptr, function.step,
		                                                  function.final, function.value, function.inverse, nullptr),
		                   db);
	}
}

} // namespace duckdb
