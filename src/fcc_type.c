// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/type.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "fcc/layout.h"

static bool fcc_type_context_ensure_type_capacity(FccTypeContext* context, size_t capacity) {
  size_t new_capacity;
  FccType* new_types;

  assert(context != NULL);

  if (capacity <= context->type_capacity) {
    return true;
  }

  new_capacity = context->type_capacity;
  if (new_capacity == 0) {
    new_capacity = 16;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccType))) {
    return false;
  }

  new_types = (FccType*)realloc(context->types, new_capacity * sizeof(FccType));
  if (new_types == NULL) {
    return false;
  }

  context->types = new_types;
  context->type_capacity = new_capacity;
  return true;
}

static bool fcc_type_context_ensure_parameter_capacity(FccTypeContext* context, size_t capacity) {
  size_t new_capacity;
  FccTypeId* new_parameter_type_ids;

  assert(context != NULL);

  if (capacity <= context->function_parameter_capacity) {
    return true;
  }

  new_capacity = context->function_parameter_capacity;
  if (new_capacity == 0) {
    new_capacity = 32;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccTypeId))) {
    return false;
  }

  new_parameter_type_ids =
      (FccTypeId*)realloc(context->function_parameter_type_ids, new_capacity * sizeof(FccTypeId));
  if (new_parameter_type_ids == NULL) {
    return false;
  }

  context->function_parameter_type_ids = new_parameter_type_ids;
  context->function_parameter_capacity = new_capacity;
  return true;
}

static bool fcc_type_context_ensure_record_field_capacity(FccTypeContext* context,
                                                          size_t capacity) {
  size_t new_capacity;
  FccTypeRecordField* new_fields;

  assert(context != NULL);

  if (capacity <= context->record_field_capacity) {
    return true;
  }

  new_capacity = context->record_field_capacity;
  if (new_capacity == 0) {
    new_capacity = 16;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccTypeRecordField))) {
    return false;
  }

  new_fields = (FccTypeRecordField*)realloc(context->record_fields,
                                            new_capacity * sizeof(FccTypeRecordField));
  if (new_fields == NULL) {
    return false;
  }

  context->record_fields = new_fields;
  context->record_field_capacity = new_capacity;
  return true;
}

static FccType* fcc_type_context_get_mutable(FccTypeContext* context, FccTypeId type_id) {
  assert(context != NULL);

  if ((type_id == FCC_TYPE_ID_INVALID) || (type_id > context->type_count)) {
    return NULL;
  }

  return &context->types[type_id - 1];
}

static FccTypeId fcc_type_context_append_type(FccTypeContext* context, const FccType* type) {
  assert(context != NULL);
  assert(type != NULL);

  if (!fcc_type_context_ensure_type_capacity(context, context->type_count + 1)) {
    return FCC_TYPE_ID_INVALID;
  }

  context->types[context->type_count] = *type;
  ++context->type_count;
  return (FccTypeId)context->type_count;
}

static bool fcc_type_matches(const FccTypeContext* context, const FccType* type,
                             const FccType* candidate) {
  size_t parameter_index;

  assert(context != NULL);
  assert(type != NULL);
  assert(candidate != NULL);

  if ((type->kind != candidate->kind) || (type->qualifiers != candidate->qualifiers)) {
    return false;
  }

  switch (type->kind) {
    case FCC_TYPE_POINTER:
      return type->data.pointer.pointee_type_id == candidate->data.pointer.pointee_type_id;
    case FCC_TYPE_ARRAY:
      return (type->data.array.element_type_id == candidate->data.array.element_type_id) &&
             (type->data.array.element_count == candidate->data.array.element_count) &&
             (type->data.array.is_vla == candidate->data.array.is_vla);
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
      return (type->data.record.tag_name == candidate->data.record.tag_name) &&
             (type->data.record.first_field_index == candidate->data.record.first_field_index) &&
             (type->data.record.field_count == candidate->data.record.field_count) &&
             (type->data.record.size == candidate->data.record.size) &&
             (type->data.record.alignment == candidate->data.record.alignment) &&
             (type->data.record.is_complete == candidate->data.record.is_complete);
    case FCC_TYPE_ENUM:
      return type->data.tagged.tag_name == candidate->data.tagged.tag_name;
    case FCC_TYPE_TYPEDEF:
      return (type->data.typedef_type.alias_name == candidate->data.typedef_type.alias_name) &&
             (type->data.typedef_type.aliased_type_id ==
              candidate->data.typedef_type.aliased_type_id);
    case FCC_TYPE_FUNCTION:
      if ((type->data.function.return_type_id != candidate->data.function.return_type_id) ||
          (type->data.function.parameter_count != candidate->data.function.parameter_count) ||
          (type->data.function.is_variadic != candidate->data.function.is_variadic)) {
        return false;
      }

      for (parameter_index = 0; parameter_index < type->data.function.parameter_count;
           ++parameter_index) {
        if (context->function_parameter_type_ids[type->data.function.first_parameter_index +
                                                 parameter_index] !=
            context->function_parameter_type_ids[candidate->data.function.first_parameter_index +
                                                 parameter_index]) {
          return false;
        }
      }

      return true;
    case FCC_TYPE_INVALID:
    case FCC_TYPE_VOID:
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
      return true;
  }

  return false;
}

static FccTypeId fcc_type_context_intern(FccTypeContext* context, const FccType* type) {
  size_t type_index;

  assert(context != NULL);
  assert(type != NULL);

  for (type_index = 0; type_index < context->type_count; ++type_index) {
    if (fcc_type_matches(context, &context->types[type_index], type)) {
      return (FccTypeId)(type_index + 1);
    }
  }

  return fcc_type_context_append_type(context, type);
}

static FccTypeId fcc_type_resolve_typedef_id(const FccTypeContext* context, FccTypeId type_id);

static FccTypeKind fcc_type_kind_from_id(const FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;

  type_id = fcc_type_resolve_typedef_id(context, type_id);
  type = fcc_type_context_get(context, type_id);
  if (type == NULL) {
    return FCC_TYPE_INVALID;
  }

  return type->kind;
}

static FccTypeId fcc_type_resolve_typedef_id(const FccTypeContext* context, FccTypeId type_id) {
  size_t depth;

  assert(context != NULL);

  for (depth = 0; depth < 64; ++depth) {
    const FccType* type;

    type = fcc_type_context_get(context, type_id);
    if ((type == NULL) || (type->kind != FCC_TYPE_TYPEDEF)) {
      return type_id;
    }

    type_id = type->data.typedef_type.aliased_type_id;
  }

  return FCC_TYPE_ID_INVALID;
}

FccTypeId fcc_type_resolve_typedef(const FccTypeContext* context, FccTypeId type_id) {
  return fcc_type_resolve_typedef_id(context, type_id);
}

static const FccType* fcc_type_context_get_resolved(const FccTypeContext* context,
                                                    FccTypeId type_id) {
  assert(context != NULL);

  return fcc_type_context_get(context, fcc_type_resolve_typedef_id(context, type_id));
}

void fcc_type_context_init(FccTypeContext* context) {
  assert(context != NULL);

  context->types = NULL;
  context->type_count = 0;
  context->type_capacity = 0;
  context->function_parameter_type_ids = NULL;
  context->function_parameter_count = 0;
  context->function_parameter_capacity = 0;
  context->record_fields = NULL;
  context->record_field_count = 0;
  context->record_field_capacity = 0;
}

void fcc_type_context_dispose(FccTypeContext* context) {
  if (context == NULL) {
    return;
  }

  free(context->types);
  free(context->function_parameter_type_ids);
  free(context->record_fields);
  context->types = NULL;
  context->type_count = 0;
  context->type_capacity = 0;
  context->function_parameter_type_ids = NULL;
  context->function_parameter_count = 0;
  context->function_parameter_capacity = 0;
  context->record_fields = NULL;
  context->record_field_count = 0;
  context->record_field_capacity = 0;
}

const FccType* fcc_type_context_get(const FccTypeContext* context, FccTypeId type_id) {
  assert(context != NULL);

  if ((type_id == FCC_TYPE_ID_INVALID) || (type_id > context->type_count)) {
    return NULL;
  }

  return &context->types[type_id - 1];
}

FccTypeId fcc_type_context_get_builtin(FccTypeContext* context, FccTypeKind kind,
                                       bool is_const_qualified) {
  FccType type;

  assert(context != NULL);

  memset(&type, 0, sizeof(type));
  type.kind = kind;
  type.qualifiers = is_const_qualified ? FCC_TYPE_QUALIFIER_CONST : FCC_TYPE_QUALIFIER_NONE;
  return fcc_type_context_intern(context, &type);
}

FccTypeId fcc_type_context_qualify_const(FccTypeContext* context, FccTypeId base_type_id) {
  FccType qualified_type;
  const FccType* base_type;

  assert(context != NULL);

  base_type = fcc_type_context_get(context, base_type_id);
  if (base_type == NULL) {
    return FCC_TYPE_ID_INVALID;
  }

  qualified_type = *base_type;
  qualified_type.qualifiers |= FCC_TYPE_QUALIFIER_CONST;
  return fcc_type_context_intern(context, &qualified_type);
}

FccTypeId fcc_type_context_get_pointer(FccTypeContext* context, FccTypeId pointee_type_id) {
  FccType type;

  assert(context != NULL);

  memset(&type, 0, sizeof(type));
  type.kind = FCC_TYPE_POINTER;
  type.data.pointer.pointee_type_id = pointee_type_id;
  return fcc_type_context_intern(context, &type);
}

FccTypeId fcc_type_context_get_array(FccTypeContext* context, FccTypeId element_type_id,
                                     size_t element_count, bool is_vla) {
  FccType type;

  assert(context != NULL);

  memset(&type, 0, sizeof(type));
  type.kind = FCC_TYPE_ARRAY;
  type.data.array.element_type_id = element_type_id;
  type.data.array.element_count = element_count;
  type.data.array.is_vla = is_vla;
  return fcc_type_context_intern(context, &type);
}

FccTypeId fcc_type_context_get_function(FccTypeContext* context, FccTypeId return_type_id,
                                        const FccTypeId* parameter_type_ids, size_t parameter_count,
                                        bool is_variadic) {
  FccType type;
  size_t parameter_base;

  assert(context != NULL);

  parameter_base = context->function_parameter_count;
  if ((parameter_count != 0) &&
      !fcc_type_context_ensure_parameter_capacity(context, parameter_base + parameter_count)) {
    return FCC_TYPE_ID_INVALID;
  }

  if (parameter_count != 0) {
    memcpy(context->function_parameter_type_ids + parameter_base, parameter_type_ids,
           parameter_count * sizeof(FccTypeId));
    context->function_parameter_count += parameter_count;
  }

  memset(&type, 0, sizeof(type));
  type.kind = FCC_TYPE_FUNCTION;
  type.data.function.return_type_id = return_type_id;
  type.data.function.first_parameter_index = parameter_base;
  type.data.function.parameter_count = parameter_count;
  type.data.function.is_variadic = is_variadic;
  return fcc_type_context_intern(context, &type);
}

FccTypeId fcc_type_context_create_record(FccTypeContext* context, FccTypeKind kind,
                                         const char* tag_name) {
  FccType type;

  assert(context != NULL);
  assert((kind == FCC_TYPE_STRUCT) || (kind == FCC_TYPE_UNION));

  memset(&type, 0, sizeof(type));
  type.kind = kind;
  type.data.record.tag_name = tag_name;
  type.data.record.first_field_index = 0;
  type.data.record.field_count = 0;
  type.data.record.size = 0;
  type.data.record.alignment = 1;
  type.data.record.is_complete = false;
  return fcc_type_context_append_type(context, &type);
}

FccTypeId fcc_type_context_create_enum(FccTypeContext* context, const char* tag_name) {
  FccType type;

  assert(context != NULL);

  memset(&type, 0, sizeof(type));
  type.kind = FCC_TYPE_ENUM;
  type.data.tagged.tag_name = tag_name;
  return fcc_type_context_append_type(context, &type);
}

bool fcc_type_context_define_record(FccTypeContext* context, FccTypeId record_type_id,
                                    const FccTypeRecordFieldInput* fields, size_t field_count) {
  FccType* record_type;
  FccLayoutMember* layout_members;
  size_t first_field_index;
  size_t field_index;
  size_t record_size;
  size_t record_alignment;

  assert(context != NULL);

  if ((field_count != 0) && (fields == NULL)) {
    return false;
  }

  record_type = fcc_type_context_get_mutable(context, record_type_id);
  if ((record_type == NULL) ||
      ((record_type->kind != FCC_TYPE_STRUCT) && (record_type->kind != FCC_TYPE_UNION))) {
    return false;
  }

  if (record_type->data.record.is_complete) {
    return false;
  }

  layout_members = NULL;
  if (field_count != 0) {
    if (field_count > (SIZE_MAX / sizeof(FccLayoutMember))) {
      return false;
    }

    layout_members = (FccLayoutMember*)calloc(field_count, sizeof(FccLayoutMember));
    if (layout_members == NULL) {
      return false;
    }
  }

  for (field_index = 0; field_index < field_count; ++field_index) {
    bool size_ok;
    bool alignment_ok;

    if (fields[field_index].type_id == FCC_TYPE_ID_INVALID) {
      free(layout_members);
      return false;
    }

    layout_members[field_index].size =
        fcc_type_size_of(context, fields[field_index].type_id, &size_ok);
    layout_members[field_index].alignment =
        fcc_type_alignment_of(context, fields[field_index].type_id, &alignment_ok);
    if (!size_ok || !alignment_ok) {
      free(layout_members);
      return false;
    }
  }

  record_size = 0;
  record_alignment = 1;
  if (record_type->kind == FCC_TYPE_STRUCT) {
    if (!fcc_layout_compute_struct(layout_members, field_count, &record_size, &record_alignment)) {
      free(layout_members);
      return false;
    }
  } else {
    if (!fcc_layout_compute_union(layout_members, field_count, &record_size, &record_alignment)) {
      free(layout_members);
      return false;
    }
  }

  first_field_index = context->record_field_count;
  if ((field_count != 0) &&
      !fcc_type_context_ensure_record_field_capacity(context, first_field_index + field_count)) {
    free(layout_members);
    return false;
  }

  for (field_index = 0; field_index < field_count; ++field_index) {
    FccTypeRecordField* destination;

    destination = &context->record_fields[first_field_index + field_index];
    destination->name = fields[field_index].name;
    destination->type_id = fields[field_index].type_id;
    destination->offset = layout_members[field_index].offset;
  }

  context->record_field_count += field_count;
  record_type->data.record.first_field_index = first_field_index;
  record_type->data.record.field_count = field_count;
  record_type->data.record.size = record_size;
  record_type->data.record.alignment = record_alignment;
  record_type->data.record.is_complete = true;
  free(layout_members);
  return true;
}

const char* fcc_type_name(FccTypeKind kind) {
  switch (kind) {
    case FCC_TYPE_INVALID:
      return "invalid";
    case FCC_TYPE_VOID:
      return "void";
    case FCC_TYPE_BOOL:
      return "_Bool";
    case FCC_TYPE_CHAR:
      return "char";
    case FCC_TYPE_SIGNED_CHAR:
      return "signed char";
    case FCC_TYPE_UNSIGNED_CHAR:
      return "unsigned char";
    case FCC_TYPE_SHORT:
      return "short";
    case FCC_TYPE_UNSIGNED_SHORT:
      return "unsigned short";
    case FCC_TYPE_INT:
      return "int";
    case FCC_TYPE_UNSIGNED_INT:
      return "unsigned int";
    case FCC_TYPE_LONG:
      return "long";
    case FCC_TYPE_UNSIGNED_LONG:
      return "unsigned long";
    case FCC_TYPE_LONG_LONG:
      return "long long";
    case FCC_TYPE_UNSIGNED_LONG_LONG:
      return "unsigned long long";
    case FCC_TYPE_FLOAT:
      return "float";
    case FCC_TYPE_DOUBLE:
      return "double";
    case FCC_TYPE_LONG_DOUBLE:
      return "long double";
    case FCC_TYPE_POINTER:
      return "pointer";
    case FCC_TYPE_ARRAY:
      return "array";
    case FCC_TYPE_STRUCT:
      return "struct";
    case FCC_TYPE_UNION:
      return "union";
    case FCC_TYPE_ENUM:
      return "enum";
    case FCC_TYPE_TYPEDEF:
      return "typedef";
    case FCC_TYPE_FUNCTION:
      return "function";
  }

  return "unknown_type";
}

bool fcc_type_equals(FccTypeId left_type_id, FccTypeId right_type_id) {
  return left_type_id == right_type_id;
}

bool fcc_type_is_integer(const FccTypeContext* context, FccTypeId type_id) {
  switch (fcc_type_kind_from_id(context, type_id)) {
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
    case FCC_TYPE_ENUM:
      return true;
    case FCC_TYPE_INVALID:
    case FCC_TYPE_VOID:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
    case FCC_TYPE_POINTER:
    case FCC_TYPE_ARRAY:
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
    case FCC_TYPE_TYPEDEF:
    case FCC_TYPE_FUNCTION:
      return false;
  }

  return false;
}

bool fcc_type_is_arithmetic(const FccTypeContext* context, FccTypeId type_id) {
  FccTypeKind kind;

  kind = fcc_type_kind_from_id(context, type_id);
  if (fcc_type_is_integer(context, type_id)) {
    return true;
  }

  switch (kind) {
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
      return true;
    case FCC_TYPE_INVALID:
    case FCC_TYPE_VOID:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
    case FCC_TYPE_POINTER:
    case FCC_TYPE_ARRAY:
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
    case FCC_TYPE_ENUM:
    case FCC_TYPE_TYPEDEF:
    case FCC_TYPE_FUNCTION:
    case FCC_TYPE_BOOL:
      return false;
  }

  return false;
}

bool fcc_type_is_unsigned_integer(const FccTypeContext* context, FccTypeId type_id) {
  switch (fcc_type_kind_from_id(context, type_id)) {
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_UNSIGNED_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
      return true;
    case FCC_TYPE_INVALID:
    case FCC_TYPE_VOID:
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
    case FCC_TYPE_POINTER:
    case FCC_TYPE_ARRAY:
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
    case FCC_TYPE_ENUM:
    case FCC_TYPE_TYPEDEF:
    case FCC_TYPE_FUNCTION:
      return false;
  }

  return false;
}

bool fcc_type_is_object(const FccTypeContext* context, FccTypeId type_id) {
  switch (fcc_type_kind_from_id(context, type_id)) {
    case FCC_TYPE_INVALID:
    case FCC_TYPE_VOID:
    case FCC_TYPE_FUNCTION:
      return false;
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
    case FCC_TYPE_POINTER:
    case FCC_TYPE_ARRAY:
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
    case FCC_TYPE_ENUM:
    case FCC_TYPE_TYPEDEF:
      return true;
  }

  return false;
}

bool fcc_type_is_scalar(const FccTypeContext* context, FccTypeId type_id) {
  return fcc_type_is_integer(context, type_id) || fcc_type_is_pointer(context, type_id);
}

bool fcc_type_is_pointer(const FccTypeContext* context, FccTypeId type_id) {
  return fcc_type_kind_from_id(context, type_id) == FCC_TYPE_POINTER;
}

bool fcc_type_is_array(const FccTypeContext* context, FccTypeId type_id) {
  return fcc_type_kind_from_id(context, type_id) == FCC_TYPE_ARRAY;
}

bool fcc_type_is_function(const FccTypeContext* context, FccTypeId type_id) {
  return fcc_type_kind_from_id(context, type_id) == FCC_TYPE_FUNCTION;
}

bool fcc_type_is_complete(const FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;

  assert(context != NULL);

  type = fcc_type_context_get_resolved(context, type_id);
  if (type == NULL) {
    return false;
  }

  switch (type->kind) {
    case FCC_TYPE_INVALID:
    case FCC_TYPE_VOID:
    case FCC_TYPE_FUNCTION:
      return false;
    case FCC_TYPE_ARRAY:
      if (type->data.array.is_vla) {
        return false;
      }

      return fcc_type_is_complete(context, type->data.array.element_type_id);
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
      return type->data.record.is_complete;
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
    case FCC_TYPE_POINTER:
    case FCC_TYPE_ENUM:
    case FCC_TYPE_TYPEDEF:
      return true;
  }

  return false;
}

size_t fcc_type_record_field_count(const FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;

  assert(context != NULL);

  type = fcc_type_context_get_resolved(context, type_id);
  if ((type == NULL) || ((type->kind != FCC_TYPE_STRUCT) && (type->kind != FCC_TYPE_UNION)) ||
      !type->data.record.is_complete) {
    return 0;
  }

  return type->data.record.field_count;
}

const FccTypeRecordField* fcc_type_record_field_at(const FccTypeContext* context, FccTypeId type_id,
                                                   size_t field_index) {
  const FccType* type;

  assert(context != NULL);

  type = fcc_type_context_get_resolved(context, type_id);
  if ((type == NULL) || ((type->kind != FCC_TYPE_STRUCT) && (type->kind != FCC_TYPE_UNION)) ||
      !type->data.record.is_complete || (field_index >= type->data.record.field_count)) {
    return NULL;
  }

  return &context->record_fields[type->data.record.first_field_index + field_index];
}

const FccTypeRecordField* fcc_type_record_find_field(const FccTypeContext* context,
                                                     FccTypeId type_id, const char* field_name) {
  size_t field_index;

  assert(context != NULL);
  assert(field_name != NULL);

  for (field_index = 0; field_index < fcc_type_record_field_count(context, type_id);
       ++field_index) {
    const FccTypeRecordField* field;

    field = fcc_type_record_field_at(context, type_id, field_index);
    if ((field != NULL) && (field->name != NULL) && (strcmp(field->name, field_name) == 0)) {
      return field;
    }
  }

  return NULL;
}

bool fcc_type_is_const_qualified(const FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;
  FccTypeId resolved_type_id;

  type = fcc_type_context_get(context, type_id);
  if (type == NULL) {
    return false;
  }

  if ((type->qualifiers & FCC_TYPE_QUALIFIER_CONST) != 0U) {
    return true;
  }

  resolved_type_id = fcc_type_resolve_typedef_id(context, type_id);
  if ((resolved_type_id == FCC_TYPE_ID_INVALID) || (resolved_type_id == type_id)) {
    return false;
  }

  type = fcc_type_context_get(context, resolved_type_id);
  return (type != NULL) && ((type->qualifiers & FCC_TYPE_QUALIFIER_CONST) != 0U);
}

FccTypeId fcc_type_decay_array(FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;
  FccTypeId resolved_type_id;

  assert(context != NULL);

  resolved_type_id = fcc_type_resolve_typedef_id(context, type_id);
  type = fcc_type_context_get(context, resolved_type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_ARRAY)) {
    return resolved_type_id;
  }

  return fcc_type_context_get_pointer(context, type->data.array.element_type_id);
}

FccTypeId fcc_type_get_pointee_type(const FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;

  type = fcc_type_context_get_resolved(context, type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_POINTER)) {
    return FCC_TYPE_ID_INVALID;
  }

  return type->data.pointer.pointee_type_id;
}

FccTypeId fcc_type_get_function_return_type(const FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;

  type = fcc_type_context_get_resolved(context, type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_FUNCTION)) {
    return FCC_TYPE_ID_INVALID;
  }

  return type->data.function.return_type_id;
}

size_t fcc_type_get_function_parameter_count(const FccTypeContext* context, FccTypeId type_id) {
  const FccType* type;

  type = fcc_type_context_get_resolved(context, type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_FUNCTION)) {
    return 0;
  }

  return type->data.function.parameter_count;
}

FccTypeId fcc_type_get_function_parameter_type(const FccTypeContext* context, FccTypeId type_id,
                                               size_t parameter_index) {
  const FccType* type;

  type = fcc_type_context_get_resolved(context, type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_FUNCTION) ||
      (parameter_index >= type->data.function.parameter_count)) {
    return FCC_TYPE_ID_INVALID;
  }

  return context
      ->function_parameter_type_ids[type->data.function.first_parameter_index + parameter_index];
}

size_t fcc_type_size_of(const FccTypeContext* context, FccTypeId type_id, bool* ok_out) {
  const FccType* type;
  bool ok;
  size_t element_size;

  assert(context != NULL);

  ok = true;
  type = fcc_type_context_get_resolved(context, type_id);
  if (type == NULL) {
    ok = false;
    if (ok_out != NULL) {
      *ok_out = false;
    }

    return 0;
  }

  switch (type->kind) {
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 1;
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 2;
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_ENUM:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 4;
    case FCC_TYPE_POINTER:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 8;
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
      if (!type->data.record.is_complete) {
        ok = false;
        break;
      }

      if (ok_out != NULL) {
        *ok_out = true;
      }

      return type->data.record.size;
    case FCC_TYPE_ARRAY:
      if (type->data.array.is_vla) {
        ok = false;
        break;
      }

      element_size = fcc_type_size_of(context, type->data.array.element_type_id, &ok);
      if (!ok) {
        break;
      }

      if (type->data.array.element_count > (SIZE_MAX / element_size)) {
        ok = false;
        break;
      }

      if (ok_out != NULL) {
        *ok_out = true;
      }

      return type->data.array.element_count * element_size;
    case FCC_TYPE_VOID:
    case FCC_TYPE_INVALID:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
    case FCC_TYPE_TYPEDEF:
    case FCC_TYPE_FUNCTION:
      ok = false;
      break;
  }

  if (ok_out != NULL) {
    *ok_out = false;
  }

  return 0;
}

size_t fcc_type_alignment_of(const FccTypeContext* context, FccTypeId type_id, bool* ok_out) {
  const FccType* type;
  bool ok;
  size_t alignment;

  assert(context != NULL);

  ok = true;
  type = fcc_type_context_get_resolved(context, type_id);
  if (type == NULL) {
    ok = false;
    if (ok_out != NULL) {
      *ok_out = false;
    }

    return 0;
  }

  switch (type->kind) {
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 1;
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 2;
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_ENUM:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 4;
    case FCC_TYPE_POINTER:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
      if (ok_out != NULL) {
        *ok_out = true;
      }

      return 8;
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
      if (!type->data.record.is_complete) {
        ok = false;
        break;
      }

      if (ok_out != NULL) {
        *ok_out = true;
      }

      return type->data.record.alignment;
    case FCC_TYPE_ARRAY:
      alignment = fcc_type_alignment_of(context, type->data.array.element_type_id, &ok);
      if (ok_out != NULL) {
        *ok_out = ok;
      }

      return alignment;
    case FCC_TYPE_VOID:
    case FCC_TYPE_INVALID:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
    case FCC_TYPE_TYPEDEF:
    case FCC_TYPE_FUNCTION:
      ok = false;
      break;
  }

  if (ok_out != NULL) {
    *ok_out = false;
  }

  return 0;
}
