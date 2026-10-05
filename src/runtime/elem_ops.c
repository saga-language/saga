/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 *
 * Slot operations for the counted kinds and interface boxes, whose slot
 * holds the pointer.
 * Struct elements get theirs from codegen, which generates the field walk.
 */

#include "runtime_internal.h"

static void string_slot_retain(void *slot) {
  saga_retain_string(*(saga_runtime_string **)slot);
}

static void string_slot_release(void *slot) {
  saga_release_string(*(saga_runtime_string **)slot);
}

static void array_slot_retain(void *slot) {
  saga_retain_array(*(saga_runtime_array **)slot);
}

static void array_slot_release(void *slot) {
  saga_release_array(*(saga_runtime_array **)slot);
}

static void map_slot_retain(void *slot) {
  saga_retain_map(*(saga_runtime_map **)slot);
}

static void map_slot_release(void *slot) {
  saga_release_map(*(saga_runtime_map **)slot);
}

static void box_slot_retain(void *slot) {
  saga_box_retain(*(saga_runtime_box **)slot);
}

static void box_slot_release(void *slot) {
  saga_box_release(*(saga_runtime_box **)slot);
}

const saga_runtime_elem_ops saga_string_elem_ops = {string_slot_retain,
                                                    string_slot_release};
const saga_runtime_elem_ops saga_array_elem_ops = {array_slot_retain,
                                                   array_slot_release};
const saga_runtime_elem_ops saga_map_elem_ops = {map_slot_retain,
                                                 map_slot_release};
const saga_runtime_elem_ops saga_box_elem_ops = {box_slot_retain,
                                                 box_slot_release};
