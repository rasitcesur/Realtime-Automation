#pragma once
#include <jni.h>
#include <cstdint>
#include <cstdio>
namespace alcybe {
inline void throwJava(JNIEnv* env, const char* type, const char* message) {
    jclass classRef = env->FindClass(type);
    if (classRef) { env->ThrowNew(classRef, message); env->DeleteLocalRef(classRef); }
}
inline void store32(int32_t* target, int32_t value) { __atomic_store_n(target, value, __ATOMIC_RELEASE); }
inline void store64(int64_t* target, int64_t value) { __atomic_store_n(target, value, __ATOMIC_RELEASE); }
inline int32_t load32(const int32_t* target) { return __atomic_load_n(target, __ATOMIC_ACQUIRE); }
inline void add32(int32_t* target, int32_t amount) { __atomic_add_fetch(target, amount, __ATOMIC_ACQ_REL); }
inline void add64(int64_t* target, int64_t amount) { __atomic_add_fetch(target, amount, __ATOMIC_ACQ_REL); }
inline void clearException(JNIEnv* env) {
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}
template<class T> T* fromHandle(jlong handle) { return reinterpret_cast<T*>(static_cast<intptr_t>(handle)); }
}
