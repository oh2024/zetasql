//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/reference_impl/type_parameter_constraints.h"

#include "zetasql/public/functions/string.h"
#include "absl/strings/substitute.h"

namespace zetasql {

constexpr int64_t kMaxStringLength = std::numeric_limits<int64_t>::max();
constexpr int64_t kMaxBytesLength = std::numeric_limits<int64_t>::max();

namespace {

absl::Status CheckMaxLength(int64_t max_length, int64_t length,
                            absl::string_view type_name) {
  if (max_length < length) {
    return absl::InvalidArgumentError(absl::Substitute(
        "$0 has maximum length $1 but got a value with length $2", type_name,
        max_length, length));
  }

  return absl::OkStatus();
}

template <typename T>
zetasql_base::StatusOr<T> ApplyPrecisionScale(T value, int64_t precision,
                                      int64_t scale) {
  static_assert(std::is_same_v<T, NumericValue> ||
                std::is_same_v<T, BigNumericValue>);
  if (value.HasFractionalPart()) {
    ZETASQL_ASSIGN_OR_RETURN(value, value.Round(scale));
  }
  // The upper and lower bounds follow this format:
  //         <----precision---->
  //   [+/-] 9999999.99999999999
  //                 <--scale-->
  // It is not possible to represent the bounds of acceptable values using a
  // double, so create a string that represents the upper bound.
  std::string upper_bound_str;
  if (scale == 0) {
    upper_bound_str.resize(precision, '9');
  } else {
    upper_bound_str.resize(precision + 1, '9');
    upper_bound_str[precision - scale] = '.';
  }
  // Unit tests ensure that no invalid values can be generated by any
  // combination of precision and scale.
  T upper_bound = T::FromString(upper_bound_str).value();
  T lower_bound = T::FromString(absl::StrCat("-", upper_bound_str)).value();

  std::string type_name;
  if constexpr (std::is_same_v<T, NumericValue>) {
    type_name = "NUMERIC";
  } else {
    type_name = "BIGNUMERIC";
  }
  if (!(lower_bound <= value && value <= upper_bound)) {
    return absl::InvalidArgumentError(absl::Substitute(
        "$0 has precision $1 and scale $2 but got a value that is "
        "not in range of [$3, $4]",
        type_name, precision, scale, lower_bound.ToString(),
        upper_bound.ToString()));
  }
  return value;
}

}  // namespace

absl::Status ApplyConstraints(Value& value, const TypeParameters& type_params,
                              ProductMode mode) {
  if (type_params.IsEmpty() || value.is_null()) {
    return absl::OkStatus();
  }
  ZETASQL_RETURN_IF_ERROR(
      value.type()->ValidateResolvedTypeParameters(type_params, mode));
  switch (value.type()->kind()) {
    case TYPE_STRING: {
      int64_t max_length =
          type_params.string_type_parameters().has_is_max_length()
              ? kMaxStringLength
              : type_params.string_type_parameters().max_length();

      // Get UTF string length.
      int64_t string_length;
      absl::Status length_error;
      if (!functions::LengthUtf8(value.string_value(), &string_length,
                                 &length_error)) {
        return length_error;
      }
      return CheckMaxLength(max_length, string_length, "STRING");
    }
    case TYPE_BYTES: {
      int64_t max_length =
          type_params.string_type_parameters().has_is_max_length()
              ? kMaxBytesLength
              : type_params.string_type_parameters().max_length();
      return CheckMaxLength(max_length, value.bytes_value().size(), "BYTES");
    }
    case TYPE_NUMERIC: {
      ZETASQL_ASSIGN_OR_RETURN(
          NumericValue updated_numeric_value,
          ApplyPrecisionScale(value.numeric_value(),
                              type_params.numeric_type_parameters().precision(),
                              type_params.numeric_type_parameters().scale()));
      value = Value::Numeric(updated_numeric_value);
      return absl::OkStatus();
    }
    case TYPE_BIGNUMERIC: {
      BigNumericValue updated_bignumeric_value;
      if (type_params.numeric_type_parameters().has_is_max_precision()) {
        if (!type_params.numeric_type_parameters().has_scale()) {
          return absl::OkStatus();
        }
        ZETASQL_ASSIGN_OR_RETURN(updated_bignumeric_value,
                         value.bignumeric_value().Round(
                             type_params.numeric_type_parameters().scale()));
      } else {
        ZETASQL_ASSIGN_OR_RETURN(updated_bignumeric_value,
                         ApplyPrecisionScale(
                             value.bignumeric_value(),
                             type_params.numeric_type_parameters().precision(),
                             type_params.numeric_type_parameters().scale()));
      }
      value = Value::BigNumeric(updated_bignumeric_value);
      return absl::OkStatus();
    }
    case TYPE_STRUCT: {
      std::vector<Value> new_fields;
      new_fields.reserve(value.num_fields());
      for (int i = 0; i < value.num_fields(); ++i) {
        Value new_field_value = value.field(i);
        ZETASQL_RETURN_IF_ERROR(
            ApplyConstraints(new_field_value, type_params.child(i), mode));
        new_fields.push_back(new_field_value);
      }
      value = Value::Struct(value.type()->AsStruct(), new_fields);
      return absl::OkStatus();
    }
    case TYPE_ARRAY: {
      std::vector<Value> new_elements;
      new_elements.reserve(value.num_elements());
      for (Value new_element_value : value.elements()) {
        ZETASQL_RETURN_IF_ERROR(
            ApplyConstraints(new_element_value, type_params.child(0), mode));
        new_elements.push_back(new_element_value);
      }
      value = Value::Array(value.type()->AsArray(), new_elements);
      return absl::OkStatus();
    }
    default:
      return absl::OkStatus();
  }
}

}  // namespace zetasql
