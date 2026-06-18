/**
 * @file MassiveAssert.h
 * @brief 自定义断言宏
 *
 * Debug:    fprintf(stderr) → __debugbreak() → abort()
 * Release:  fprintf(stderr) → 继续执行
 * 绝对不要用标准 assert() —— Release 下被完全移除
 */
#pragma once

#include <cstdio>
#include <cstdlib>

#ifdef NDEBUG
  /** @brief Release: 记录错误后继续执行 */
  #define MASSIVE_ASSERT(cond, msg) \
      do \
      { \
          if (!(cond)) \
          { \
              fprintf(stderr, \
                  "[MASSIVE_ASSERT] %s:%d — %s\n", __FILE__, __LINE__, msg); \
          } \
      } while (0)

  /** @brief Release: 带格式的错误记录，继续执行 */
  #define MASSIVE_ASSERT_FMT(cond, msg, ...) \
      do \
      { \
          if (!(cond)) \
          { \
              fprintf(stderr, \
                  "[MASSIVE_ASSERT] %s:%d — " msg "\n", \
                  __FILE__, __LINE__, __VA_ARGS__); \
          } \
      } while (0)

#else
  /** @brief Debug: 打印 → debugbreak → abort */
  #define MASSIVE_ASSERT(cond, msg) \
      do \
      { \
          if (!(cond)) \
          { \
              fprintf(stderr, \
                  "[MASSIVE_ASSERT] %s:%d — %s\n", __FILE__, __LINE__, msg); \
              __debugbreak(); \
              std::abort(); \
          } \
      } while (0)

  /** @brief Debug: 带格式的断言，打印 → debugbreak → abort */
  #define MASSIVE_ASSERT_FMT(cond, msg, ...) \
      do \
      { \
          if (!(cond)) \
          { \
              fprintf(stderr, \
                  "[MASSIVE_ASSERT] %s:%d — " msg "\n", \
                  __FILE__, __LINE__, __VA_ARGS__); \
              __debugbreak(); \
              std::abort(); \
          } \
      } while (0)

#endif
