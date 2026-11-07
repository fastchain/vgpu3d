/* minimal excpt.h for CRT-less user-mode builds (the real one needs the MSVC CRT headers) */
#pragma once
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER       1
#define EXCEPTION_CONTINUE_SEARCH       0
#define EXCEPTION_CONTINUE_EXECUTION    (-1)
#endif
typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution, ExceptionContinueSearch, ExceptionNestedException, ExceptionCollidedUnwind
} EXCEPTION_DISPOSITION;
struct _EXCEPTION_RECORD; struct _CONTEXT; struct _DISPATCHER_CONTEXT;
