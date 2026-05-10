; ModuleID = 'app'
source_filename = "app"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

%saga_runtime_string = type { ptr, i64, i64 }
%"saga.lib__Box<String>" = type { ptr }
%"saga.lib__Box<Int>" = type { i64 }

@.str = private unnamed_addr constant [5 x i8] c"hello", align 1
@.saga_runtime_str = private unnamed_addr constant %saga_runtime_string { ptr @.str, i64 5, i64 -1 }

declare void @saga_intrinsic_print(ptr)

declare ptr @saga_string_concat(ptr, ptr)

declare i64 @saga_string_compare(ptr, ptr)

declare ptr @saga_int_to_string(i64)

declare ptr @saga_float_to_string(double)

declare ptr @saga_bool_to_string(i64)

declare i64 @saga_int_hash(i64)

declare i64 @saga_string_hash(ptr)

declare i64 @saga_bool_hash(i64)

declare ptr @saga_string_lower(ptr)

declare ptr @saga_string_upper(ptr)

declare ptr @saga_string_trim(ptr)

declare ptr @saga_string_capitalize(ptr)

declare ptr @saga_string_title(ptr)

declare i64 @saga_string_has_prefix(ptr, ptr)

declare i64 @saga_string_has_suffix(ptr, ptr)

declare i64 @saga_string_contains(ptr, ptr)

declare ptr @saga_string_split(ptr, ptr)

declare ptr @saga_string_bytes(ptr)

declare i64 @saga_string_count(ptr)

declare ptr @saga_string_runes(ptr)

declare i64 @saga_string_to_int(ptr, ptr)

declare i64 @saga_string_to_float(ptr, ptr)

declare ptr @saga_string_format(ptr, ptr)

declare ptr @saga_int_format(i64, ptr)

declare ptr @saga_float_format(double, ptr)

declare ptr @saga_array_new(i64, i64)

declare void @saga_array_push(ptr, ptr)

declare ptr @saga_array_at(ptr, i64)

declare i64 @saga_array_size(ptr)

declare i64 @saga_array_find(ptr, ptr, ptr)

declare void @saga_array_insert(ptr, ptr, i64)

declare ptr @saga_array_pop(ptr)

declare void @saga_array_set(ptr, i64, ptr)

declare i64 @saga_array_equals(ptr, ptr)

declare ptr @saga_array_clone(ptr)

declare ptr @saga_range_to_array(i64, i64)

declare ptr @saga_string_at(ptr, i64)

declare ptr @saga_string_slice(ptr, i64, i64)

declare void @saga_retain_string(ptr)

declare void @saga_release_string(ptr)

declare void @saga_retain_array(ptr)

declare void @saga_release_array(ptr)

declare ptr @saga_map_new(i64, i64, i64, ptr)

declare void @saga_map_set(ptr, ptr, ptr)

declare ptr @saga_map_get(ptr, ptr)

declare i64 @saga_map_has(ptr, ptr)

declare void @saga_map_remove(ptr, ptr)

declare i64 @saga_map_size(ptr)

declare ptr @saga_map_key_at(ptr, i64)

declare ptr @saga_map_value_at(ptr, i64)

declare ptr @saga_map_keys(ptr)

declare i64 @saga_map_equals(ptr, ptr)

declare void @saga_retain_map(ptr)

declare void @saga_release_map(ptr)

declare void @saga_executor_init(i64)

declare void @saga_executor_shutdown()

declare ptr @saga_executor_spawn(ptr, ptr, i64, i64)

declare void @saga_executor_schedule(ptr)

declare ptr @saga_channel_new(i64, i64)

declare i32 @saga_channel_recv(ptr, ptr)

declare void @saga_channel_close(ptr)

declare void @saga_channel_destroy(ptr)

declare i64 @saga_task_alive(ptr)

declare void @saga_task_cancel(ptr)

declare void @saga_task_term(ptr)

declare ptr @saga_task_wait(ptr, ptr)

declare void @saga_task_drop(ptr)

declare i64 @saga_context_cancelled(ptr)

declare void @saga_context_exit(ptr, ptr, i64)

declare i32 @saga_context_send(ptr, ptr)

declare void @saga_reduction_tick(ptr)

declare void @saga_actor_yield()

declare void @saga_actor_trap(ptr)

declare ptr @saga_error_from_trap(ptr)

declare void @io__Println(ptr)

define i32 @main() {
entry:
  %s = alloca %"saga.lib__Box<String>", align 8
  %Box.lit3 = alloca %"saga.lib__Box<String>", align 8
  %b = alloca %"saga.lib__Box<Int>", align 8
  %Box.lit = alloca %"saga.lib__Box<Int>", align 8
  store %"saga.lib__Box<Int>" zeroinitializer, ptr %Box.lit, align 8
  %value = getelementptr inbounds %"saga.lib__Box<Int>", ptr %Box.lit, i32 0, i32 0
  store i64 42, ptr %value, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %b, ptr align 8 %Box.lit, i64 8, i1 false)
  %b1 = load %"saga.lib__Box<Int>", ptr %b, align 8
  %mcall = call i64 @gen__app__Box__Get__Int(%"saga.lib__Box<Int>" %b1)
  %mcall2 = call ptr @int__Int__String(i64 %mcall)
  call void @io__Println(ptr %mcall2)
  store %"saga.lib__Box<String>" zeroinitializer, ptr %Box.lit3, align 8
  %value4 = getelementptr inbounds %"saga.lib__Box<String>", ptr %Box.lit3, i32 0, i32 0
  store ptr @.saga_runtime_str, ptr %value4, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %s, ptr align 8 %Box.lit3, i64 8, i1 false)
  %s5 = load %"saga.lib__Box<String>", ptr %s, align 8
  %mcall6 = call ptr @gen__app__Box__Get__String(%"saga.lib__Box<String>" %s5)
  call void @io__Println(ptr %mcall6)
  ret i32 0
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg) #0

define linkonce_odr i64 @gen__app__Box__Get__Int(ptr %b) {
entry:
  %b1 = alloca ptr, align 8
  store ptr %b, ptr %b1, align 8
  %b2 = load ptr, ptr %b1, align 8
  %value = getelementptr inbounds %"saga.lib__Box<Int>", ptr %b2, i32 0, i32 0
  %value3 = load i64, ptr %value, align 8
  ret i64 %value3
}

declare ptr @int__Int__String(i64)

define linkonce_odr ptr @gen__app__Box__Get__String(ptr %b) {
entry:
  %b1 = alloca ptr, align 8
  store ptr %b, ptr %b1, align 8
  %b2 = load ptr, ptr %b1, align 8
  %value = getelementptr inbounds %"saga.lib__Box<String>", ptr %b2, i32 0, i32 0
  %value3 = load ptr, ptr %value, align 8
  ret ptr %value3
}

attributes #0 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
