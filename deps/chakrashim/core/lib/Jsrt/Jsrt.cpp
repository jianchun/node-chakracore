//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "JsrtPch.h"
#include "JsrtInternal.h"
#include "JsrtExternalObject.h"
#include "JsrtExternalArrayBuffer.h"
#include "jsrtHelper.h"

#include "JsrtSourceHolder.h"
#include "ByteCode/ByteCodeSerializer.h"
#include "Common/ByteSwap.h"
#include "Library/DataView.h"
#include "Library/JavascriptSymbol.h"
#include "Base/ThreadContextTlsEntry.h"
#include "Codex/Utf8Helper.h"

// Parser Includes
#include "cmperr.h"     // For ERRnoMemory
#include "screrror.h"   // For CompileScriptException

#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
#include "TestHooksRt.h"
#endif

JsErrorCode CheckContext(JsrtContext *currentContext, bool verifyRuntimeState, bool allowInObjectBeforeCollectCallback)
{
    if (currentContext == nullptr)
    {
        return JsErrorNoCurrentContext;
    }

    Js::ScriptContext *scriptContext = currentContext->GetScriptContext();
    Assert(scriptContext != nullptr);
    Recycler *recycler = scriptContext->GetRecycler();
    ThreadContext *threadContext = scriptContext->GetThreadContext();

    // We don't need parameter check if it's checked in previous wrapper.
    if (verifyRuntimeState)
    {
        if (recycler && recycler->IsHeapEnumInProgress())
        {
            return JsErrorHeapEnumInProgress;
        }
        else if (!allowInObjectBeforeCollectCallback && recycler && recycler->IsInObjectBeforeCollectCallback())
        {
            return JsErrorInObjectBeforeCollectCallback;
        }
        else if (threadContext->IsExecutionDisabled())
        {
            return JsErrorInDisabledState;
        }
        else if (scriptContext->IsInProfileCallback())
        {
            return JsErrorInProfileCallback;
        }
        else if (threadContext->IsInThreadServiceCallback())
        {
            return JsErrorInThreadServiceCallback;
        }

        // Make sure we don't have an outstanding exception.
        if (scriptContext->GetThreadContext()->GetRecordedException() != nullptr)
        {
            return JsErrorInExceptionState;
        }
    }

    return JsNoError;
}

CHAKRA_API JsCreateRuntime(_In_ JsRuntimeAttributes attributes, _In_opt_ JsThreadServiceCallback threadService, _Out_ JsRuntimeHandle *runtimeHandle)
{
    return GlobalAPIWrapper([&] () -> JsErrorCode {
        PARAM_NOT_NULL(runtimeHandle);
        *runtimeHandle = nullptr;

        const JsRuntimeAttributes JsRuntimeAttributesAll =
            (JsRuntimeAttributes)(
            JsRuntimeAttributeDisableBackgroundWork |
            JsRuntimeAttributeAllowScriptInterrupt |
            JsRuntimeAttributeEnableIdleProcessing |
            JsRuntimeAttributeDisableEval |
            JsRuntimeAttributeDisableNativeCodeGeneration |
            JsRuntimeAttributeEnableExperimentalFeatures |
            JsRuntimeAttributeDispatchSetExceptionsToDebugger |
            JsRuntimeAttributeEnableSimdjsFeature
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
            | JsRuntimeAttributeSerializeLibraryByteCode
#endif
        );

        Assert((attributes & ~JsRuntimeAttributesAll) == 0);
        if ((attributes & ~JsRuntimeAttributesAll) != 0)
        {
            return JsErrorInvalidArgument;
        }

        AllocationPolicyManager * policyManager = HeapNew(AllocationPolicyManager, (attributes & JsRuntimeAttributeDisableBackgroundWork) == 0);
        bool enableExperimentalFeatures = (attributes & JsRuntimeAttributeEnableExperimentalFeatures) != 0;
        bool enableSimdjsFeature = (attributes & JsRuntimeAttributeEnableSimdjsFeature) != 0;
        ThreadContext * threadContext = HeapNew(ThreadContext, policyManager, threadService, enableExperimentalFeatures, enableSimdjsFeature);

        if (((attributes & JsRuntimeAttributeDisableBackgroundWork) != 0)
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
            && !Js::Configuration::Global.flags.ConcurrentRuntime
#endif
            )
        {
            threadContext->OptimizeForManyInstances(true);
#if ENABLE_NATIVE_CODEGEN
            threadContext->EnableBgJit(false);
#endif
        }

        if (!threadContext->IsRentalThreadingEnabledInJSRT()
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
            || Js::Configuration::Global.flags.DisableRentalThreading
#endif
            )
        {
            threadContext->SetIsThreadBound();
        }

        if (attributes & JsRuntimeAttributeAllowScriptInterrupt)
        {
            threadContext->SetThreadContextFlag(ThreadContextFlagCanDisableExecution);
        }

        if (attributes & JsRuntimeAttributeDisableEval)
        {
            threadContext->SetThreadContextFlag(ThreadContextFlagEvalDisabled);
        }

        if (attributes & JsRuntimeAttributeDisableNativeCodeGeneration)
        {
            threadContext->SetThreadContextFlag(ThreadContextFlagNoJIT);
        }

#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
        if (Js::Configuration::Global.flags.PrimeRecycler)
        {
            threadContext->EnsureRecycler()->Prime();
        }
#endif

        bool enableIdle = (attributes & JsRuntimeAttributeEnableIdleProcessing) == JsRuntimeAttributeEnableIdleProcessing;
        bool dispatchExceptions = (attributes & JsRuntimeAttributeDispatchSetExceptionsToDebugger) == JsRuntimeAttributeDispatchSetExceptionsToDebugger;

        JsrtRuntime * runtime = HeapNew(JsrtRuntime, threadContext, enableIdle, dispatchExceptions);
        threadContext->SetCurrentThreadId(ThreadContext::NoThread);
        *runtimeHandle = runtime->ToHandle();
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
        runtime->SetSerializeByteCodeForLibrary((attributes & JsRuntimeAttributeSerializeLibraryByteCode) != 0);
#endif

        return JsNoError;
    });
}

template <CollectionFlags flags>
JsErrorCode JsCollectGarbageCommon(JsRuntimeHandle runtimeHandle)
{
    return GlobalAPIWrapper([&]() -> JsErrorCode {
        VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);

        ThreadContext * threadContext = JsrtRuntime::FromHandle(runtimeHandle)->GetThreadContext();

        if (threadContext->GetRecycler() && threadContext->GetRecycler()->IsHeapEnumInProgress())
        {
            return JsErrorHeapEnumInProgress;
        }
        else if (threadContext->IsInThreadServiceCallback())
        {
            return JsErrorInThreadServiceCallback;
        }

        ThreadContextScope scope(threadContext);

        if (!scope.IsValid())
        {
            return JsErrorWrongThread;
        }

        Recycler* recycler = threadContext->EnsureRecycler();
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
        if (flags & CollectOverride_SkipStack)
        {
            Recycler::AutoEnterExternalStackSkippingGCMode autoGC(recycler);
            recycler->CollectNow<flags>();
        }
        else
#endif
        {
            recycler->CollectNow<flags>();
        }
        return JsNoError;
    });
}

CHAKRA_API JsCollectGarbage(_In_ JsRuntimeHandle runtimeHandle)
{
    return JsCollectGarbageCommon<CollectNowExhaustive>(runtimeHandle);
}

#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
CHAKRA_API JsPrivateCollectGarbageSkipStack(_In_ JsRuntimeHandle runtimeHandle)
{
    return JsCollectGarbageCommon<CollectNowExhaustiveSkipStack>(runtimeHandle);
}
#endif

CHAKRA_API JsDisposeRuntime(_In_ JsRuntimeHandle runtimeHandle)
{
    return GlobalAPIWrapper([&] () -> JsErrorCode {
        VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);

        JsrtRuntime * runtime = JsrtRuntime::FromHandle(runtimeHandle);
        ThreadContext * threadContext = runtime->GetThreadContext();
        ThreadContextScope scope(threadContext);

        // We should not dispose if the runtime is being used.
        if (!scope.IsValid() ||
            scope.WasInUse() ||
            (threadContext->GetRecycler() && threadContext->GetRecycler()->IsHeapEnumInProgress()))
        {
            return JsErrorRuntimeInUse;
        }
        else if (threadContext->IsInThreadServiceCallback())
        {
            return JsErrorInThreadServiceCallback;
        }

        // Invoke and clear the callbacks while the contexts and runtime are still available
        {
            Recycler* recycler = threadContext->GetRecycler();
            if (recycler != nullptr)
            {
                recycler->ClearObjectBeforeCollectCallbacks();
            }
        }

        if (runtime->GetJsrtDebugManager() != nullptr)
        {
            runtime->GetJsrtDebugManager()->ClearDebuggerObjects();
        }

        // Close any open Contexts.
        // We need to do this before recycler shutdown, because ScriptEngine->Close won't work then.
        runtime->CloseContexts();

        runtime->DeleteJsrtDebugManager();

#if defined(CHECK_MEMORY_LEAK) || defined(LEAK_REPORT)
        bool doFinalGC = false;

#if defined(LEAK_REPORT)
        if (Js::Configuration::Global.flags.IsEnabled(Js::LeakReportFlag))
        {
            doFinalGC = true;
        }
#endif

#if defined(CHECK_MEMORY_LEAK)
        if (Js::Configuration::Global.flags.CheckMemoryLeak)
        {
            doFinalGC = true;
        }
#endif

        if (doFinalGC)
        {
            Recycler *recycler = threadContext->GetRecycler();
            if (recycler)
            {
                recycler->EnsureNotCollecting();
                recycler->CollectNow<CollectNowFinalGC>();
                Assert(!recycler->CollectionInProgress());
            }
        }
#endif

        runtime->SetBeforeCollectCallback(nullptr, nullptr);
        threadContext->CloseForJSRT();
        HeapDelete(threadContext);

        HeapDelete(runtime);

        scope.Invalidate();

        return JsNoError;
    });
}

CHAKRA_API JsAddRef(_In_ JsRef ref, _Out_opt_ unsigned int *count)
{
    VALIDATE_JSREF(ref);
    if (count != nullptr)
    {
        *count = 0;
    }

    if (Js::TaggedNumber::Is(ref))
    {
        // The count is always one because these are never collected
        if (count)
        {
            *count = 1;
        }
        return JsNoError;
    }

    if (JsrtContext::Is(ref))
    {
        return GlobalAPIWrapper([&] () -> JsErrorCode
        {
            Recycler * recycler = static_cast<JsrtContext *>(ref)->GetRuntime()->GetThreadContext()->GetRecycler();
            recycler->RootAddRef(ref, count);
            return JsNoError;
        });
    }
    else
    {
        ThreadContext* threadContext = ThreadContext::GetContextForCurrentThread();
        if (threadContext == nullptr)
        {
            return JsErrorNoCurrentContext;
        }
        Recycler * recycler = threadContext->GetRecycler();
        return GlobalAPIWrapper([&] () -> JsErrorCode
        {

            // Note, some references may live in arena-allocated memory, so we need to do this check
            if (!recycler->IsValidObject(ref))
            {
                return JsNoError;
            }

            recycler->RootAddRef(ref, count);
            return JsNoError;
        });
    }
}

CHAKRA_API JsRelease(_In_ JsRef ref, _Out_opt_ unsigned int *count)
{
    VALIDATE_JSREF(ref);
    if (count != nullptr)
    {
        *count = 0;
    }

    if (Js::TaggedNumber::Is(ref))
    {
        // The count is always one because these are never collected
        if (count)
        {
            *count = 1;
        }
        return JsNoError;
    }

    if (JsrtContext::Is(ref))
    {
        return GlobalAPIWrapper([&] () -> JsErrorCode
        {
            Recycler * recycler = static_cast<JsrtContext *>(ref)->GetRuntime()->GetThreadContext()->GetRecycler();
            recycler->RootRelease(ref, count);
            return JsNoError;
        });
    }
    else
    {
        ThreadContext* threadContext = ThreadContext::GetContextForCurrentThread();
        if (threadContext == nullptr)
        {
            return JsErrorNoCurrentContext;
        }
        Recycler * recycler = threadContext->GetRecycler();
        return GlobalAPIWrapper([&]() -> JsErrorCode
        {
            // Note, some references may live in arena-allocated memory, so we need to do this check
            if (!recycler->IsValidObject(ref))
            {
                return JsNoError;
            }
            recycler->RootRelease(ref, count);
            return JsNoError;
        });
    }
}


CHAKRA_API JsSetObjectBeforeCollectCallback(_In_ JsRef ref, _In_opt_ void *callbackState, _In_ JsObjectBeforeCollectCallback objectBeforeCollectCallback)
{
    VALIDATE_JSREF(ref);

    if (Js::TaggedNumber::Is(ref))
    {
        return JsErrorInvalidArgument;
    }

    if (JsrtContext::Is(ref))
    {
        return GlobalAPIWrapper([&]() -> JsErrorCode
        {
            ThreadContext* threadContext = static_cast<JsrtContext *>(ref)->GetRuntime()->GetThreadContext();
            Recycler * recycler = threadContext->GetRecycler();
            recycler->SetObjectBeforeCollectCallback(ref, reinterpret_cast<Recycler::ObjectBeforeCollectCallback>(objectBeforeCollectCallback), callbackState,
                reinterpret_cast<Recycler::ObjectBeforeCollectCallbackWrapper>(JsrtCallbackState::ObjectBeforeCallectCallbackWrapper), threadContext);
            return JsNoError;
        });
    }
    else
    {
        ThreadContext* threadContext = ThreadContext::GetContextForCurrentThread();
        if (threadContext == nullptr)
        {
            return JsErrorNoCurrentContext;
        }
        Recycler * recycler = threadContext->GetRecycler();
        return GlobalAPIWrapper([&]() -> JsErrorCode
        {
            if (!recycler->IsValidObject(ref))
            {
                return JsErrorInvalidArgument;
            }

            recycler->SetObjectBeforeCollectCallback(ref, reinterpret_cast<Recycler::ObjectBeforeCollectCallback>(objectBeforeCollectCallback), callbackState,
                reinterpret_cast<Recycler::ObjectBeforeCollectCallbackWrapper>(JsrtCallbackState::ObjectBeforeCallectCallbackWrapper), threadContext);
            return JsNoError;
        });
    }
}

CHAKRA_API JsCreateContext(_In_ JsRuntimeHandle runtimeHandle, _Out_ JsContextRef *newContext)
{
    return GlobalAPIWrapper([&]() -> JsErrorCode {
        PARAM_NOT_NULL(newContext);
        VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);

        JsrtRuntime * runtime = JsrtRuntime::FromHandle(runtimeHandle);
        ThreadContext * threadContext = runtime->GetThreadContext();

        if (threadContext->GetRecycler() && threadContext->GetRecycler()->IsHeapEnumInProgress())
        {
            return JsErrorHeapEnumInProgress;
        }
        else if (threadContext->IsInThreadServiceCallback())
        {
            return JsErrorInThreadServiceCallback;
        }

        ThreadContextScope scope(threadContext);

        if (!scope.IsValid())
        {
            return JsErrorWrongThread;
        }

        JsrtContext * context = JsrtContext::New(runtime);

        JsrtDebugManager* jsrtDebugManager = runtime->GetJsrtDebugManager();

        if (jsrtDebugManager != nullptr)
        {
            Js::ScriptContext* scriptContext = context->GetScriptContext();
            scriptContext->InitializeDebugging();

            Js::DebugContext* debugContext = scriptContext->GetDebugContext();
            debugContext->SetHostDebugContext(jsrtDebugManager);

            Js::ProbeContainer* probeContainer = debugContext->GetProbeContainer();
            probeContainer->InitializeInlineBreakEngine(jsrtDebugManager);
            probeContainer->InitializeDebuggerScriptOptionCallback(jsrtDebugManager);

            threadContext->GetDebugManager()->SetLocalsDisplayFlags(Js::DebugManager::LocalsDisplayFlags::LocalsDisplayFlags_NoGroupMethods);
        }

        *newContext = (JsContextRef)context;
        return JsNoError;
    });
}

CHAKRA_API JsGetCurrentContext(_Out_ JsContextRef *currentContext)
{
    PARAM_NOT_NULL(currentContext);

    BEGIN_JSRT_NO_EXCEPTION
    {
      *currentContext = (JsContextRef)JsrtContext::GetCurrent();
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsSetCurrentContext(_In_ JsContextRef newContext)
{
    return GlobalAPIWrapper([&] () -> JsErrorCode {
        JsrtContext *currentContext = JsrtContext::GetCurrent();
        Recycler* recycler = currentContext != nullptr ? currentContext->GetScriptContext()->GetRecycler() : nullptr;

        if (currentContext && recycler->IsHeapEnumInProgress())
        {
            return JsErrorHeapEnumInProgress;
        }
        else if (currentContext && currentContext->GetRuntime()->GetThreadContext()->IsInThreadServiceCallback())
        {
            return JsErrorInThreadServiceCallback;
        }

        if (!JsrtContext::TrySetCurrent((JsrtContext *)newContext))
        {
            return JsErrorWrongThread;
        }

        return JsNoError;
    });
}

CHAKRA_API JsGetContextOfObject(_In_ JsValueRef object, _Out_ JsContextRef *context)
{
    VALIDATE_JSREF(object);
    PARAM_NOT_NULL(context);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if(!Js::RecyclableObject::Is(object))
        {
            RETURN_NO_EXCEPTION(JsErrorArgumentNotObject);
        }
        Js::RecyclableObject* obj = Js::RecyclableObject::FromVar(object);
        *context = (JsContextRef)obj->GetScriptContext()->GetLibrary()->GetPinnedJsrtContextObject();
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsGetContextData(_In_ JsContextRef context, _Out_ void **data)
{
    VALIDATE_JSREF(context);
    PARAM_NOT_NULL(data);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (!JsrtContext::Is(context))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        *data = static_cast<JsrtContext *>(context)->GetExternalData();
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsSetContextData(_In_ JsContextRef context, _In_ void *data)
{
    VALIDATE_JSREF(context);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (!JsrtContext::Is(context))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        static_cast<JsrtContext *>(context)->SetExternalData(data);
    }
    END_JSRT_NO_EXCEPTION
}

void HandleScriptCompileError(Js::ScriptContext * scriptContext, CompileScriptException * se)
{
    HRESULT hr = se->ei.scode;
    if (hr == E_OUTOFMEMORY || hr == VBSERR_OutOfMemory || hr == VBSERR_OutOfStack || hr == ERRnoMemory)
    {
        Js::Throw::OutOfMemory();
    }

    Js::JavascriptError* error = Js::JavascriptError::CreateFromCompileScriptException(scriptContext, se);

    Js::JavascriptExceptionObject * exceptionObject = RecyclerNew(scriptContext->GetRecycler(),
        Js::JavascriptExceptionObject, error, scriptContext, nullptr);

    scriptContext->GetThreadContext()->SetRecordedException(exceptionObject);
}

CHAKRA_API JsGetUndefinedValue(_Out_ JsValueRef *undefinedValue)
{
    return ContextAPINoScriptWrapper([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(undefinedValue);

        *undefinedValue = scriptContext->GetLibrary()->GetUndefined();

        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

CHAKRA_API JsGetNullValue(_Out_ JsValueRef *nullValue)
{
    return ContextAPINoScriptWrapper([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(nullValue);

        *nullValue = scriptContext->GetLibrary()->GetNull();

        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

CHAKRA_API JsGetTrueValue(_Out_ JsValueRef *trueValue)
{
    return ContextAPINoScriptWrapper([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(trueValue);

        *trueValue = scriptContext->GetLibrary()->GetTrue();

        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

CHAKRA_API JsGetFalseValue(_Out_ JsValueRef *falseValue)
{
    return ContextAPINoScriptWrapper([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(falseValue);

        *falseValue = scriptContext->GetLibrary()->GetFalse();

        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

CHAKRA_API JsBoolToBoolean(_In_ bool value, _Out_ JsValueRef *booleanValue)
{
    return ContextAPINoScriptWrapper([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(booleanValue);

        *booleanValue = value ? scriptContext->GetLibrary()->GetTrue() :
            scriptContext->GetLibrary()->GetFalse();
        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

CHAKRA_API JsBooleanToBool(_In_ JsValueRef value, _Out_ bool *boolValue)
{
    VALIDATE_JSREF(value);
    PARAM_NOT_NULL(boolValue);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (!Js::JavascriptBoolean::Is(value))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        *boolValue = Js::JavascriptBoolean::FromVar(value)->GetValue() ? true : false;
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsConvertValueToBoolean(_In_ JsValueRef value, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(value, scriptContext);
        PARAM_NOT_NULL(result);

        if (Js::JavascriptConversion::ToBool((Js::Var)value, scriptContext))
        {
            *result = scriptContext->GetLibrary()->GetTrue();
            return JsNoError;
        }
        else
        {
            *result = scriptContext->GetLibrary()->GetFalse();
            return JsNoError;
        }
    });
}

CHAKRA_API JsGetValueType(_In_ JsValueRef value, _Out_ JsValueType *type)
{
    VALIDATE_JSREF(value);
    PARAM_NOT_NULL(type);

    BEGIN_JSRT_NO_EXCEPTION
    {
        Js::TypeId typeId = Js::JavascriptOperators::GetTypeId(value);
        switch (typeId)
        {
        case Js::TypeIds_Undefined:
            *type = JsUndefined;
            break;
        case Js::TypeIds_Null:
            *type = JsNull;
            break;
        case Js::TypeIds_Boolean:
            *type = JsBoolean;
            break;
        case Js::TypeIds_Integer:
        case Js::TypeIds_Number:
        case Js::TypeIds_Int64Number:
        case Js::TypeIds_UInt64Number:
            *type = JsNumber;
            break;
        case Js::TypeIds_String:
            *type = JsString;
            break;
        case Js::TypeIds_Function:
            *type = JsFunction;
            break;
        case Js::TypeIds_Error:
            *type = JsError;
            break;
        case Js::TypeIds_Array:
        case Js::TypeIds_NativeIntArray:
#if ENABLE_COPYONACCESS_ARRAY
        case Js::TypeIds_CopyOnAccessNativeIntArray:
#endif
        case Js::TypeIds_NativeFloatArray:
        case Js::TypeIds_ES5Array:
            *type = JsArray;
            break;
        case Js::TypeIds_Symbol:
            *type = JsSymbol;
            break;
        case Js::TypeIds_ArrayBuffer:
            *type = JsArrayBuffer;
            break;
        case Js::TypeIds_DataView:
            *type = JsDataView;
            break;
        default:
            if (Js::TypedArrayBase::Is(typeId))
            {
                *type = JsTypedArray;
            }
            else
            {
                *type = JsObject;
            }
            break;
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsDoubleToNumber(_In_ double dbl, _Out_ JsValueRef *asValue)
{
    PARAM_NOT_NULL(asValue);
    if (Js::JavascriptNumber::TryToVarFastWithCheck(dbl, asValue)) {
      return JsNoError;
    }

    return ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        *asValue = Js::JavascriptNumber::ToVarNoCheck(dbl, scriptContext);
        return JsNoError;
    });
}

CHAKRA_API JsIntToNumber(_In_ int intValue, _Out_ JsValueRef *asValue)
{
    PARAM_NOT_NULL(asValue);
    if (Js::JavascriptNumber::TryToVarFast(intValue, asValue))
    {
        return JsNoError;
    }

    return ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        *asValue = Js::JavascriptNumber::ToVar(intValue, scriptContext);
        return JsNoError;
    });
}

CHAKRA_API JsNumberToDouble(_In_ JsValueRef value, _Out_ double *asDouble)
{
    VALIDATE_JSREF(value);
    PARAM_NOT_NULL(asDouble);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (Js::TaggedInt::Is(value))
        {
            *asDouble = Js::TaggedInt::ToDouble(value);
        }
        else if (Js::JavascriptNumber::Is_NoTaggedIntCheck(value))
        {
            *asDouble = Js::JavascriptNumber::GetValue(value);
        }
        else
        {
            *asDouble = 0;
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsNumberToInt(_In_ JsValueRef value, _Out_ int *asInt)
{
    VALIDATE_JSREF(value);
    PARAM_NOT_NULL(asInt);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (Js::TaggedInt::Is(value))
        {
            *asInt = Js::TaggedInt::ToInt32(value);
        }
        else if (Js::JavascriptNumber::Is_NoTaggedIntCheck(value))
        {
            *asInt = Js::JavascriptConversion::ToInt32(Js::JavascriptNumber::GetValue(value));
        }
        else
        {
            *asInt = 0;
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsConvertValueToNumber(_In_ JsValueRef value, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(value, scriptContext);
        PARAM_NOT_NULL(result);

        *result = (JsValueRef)Js::JavascriptOperators::ToNumber((Js::Var)value, scriptContext);
        return JsNoError;
    });
}

CHAKRA_API JsGetStringLength(_In_ JsValueRef value, _Out_ int *length)
{
    VALIDATE_JSREF(value);
    PARAM_NOT_NULL(length);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (!Js::JavascriptString::Is(value))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        *length = Js::JavascriptString::FromVar(value)->GetLengthAsSignedInt();
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsPointerToString(_In_reads_(stringLength) const wchar_t *stringValue, _In_ size_t stringLength, _Out_ JsValueRef *string)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(stringValue);
        PARAM_NOT_NULL(string);

        if (!Js::IsValidCharCount(stringLength))
        {
            Js::JavascriptError::ThrowOutOfMemoryError(scriptContext);
        }

        *string = Js::JavascriptString::NewCopyBuffer(stringValue, static_cast<charcount_t>(stringLength), scriptContext);
        return JsNoError;
    });
}

CHAKRA_API JsPointerToStringUtf8(_In_reads_(stringLength) const char *stringValue, _In_ size_t stringLength, _Out_ JsValueRef *string)
{
    PARAM_NOT_NULL(stringValue);
    utf8::NarrowToWide wstr(stringValue, stringLength);
    if (!wstr)
    {
        return JsErrorOutOfMemory;
    }

    return JsPointerToString(wstr, wstr.Length(), string);
}

// TODO: The annotation of stringPtr is wrong.  Need to fix definition in chakrart.h
// The warning is '*stringPtr' could be '0' : this does not adhere to the specification for the function 'JsStringToPointer'.
#pragma warning(suppress:6387)
CHAKRA_API JsStringToPointer(_In_ JsValueRef stringValue, _Outptr_result_buffer_(*stringLength) const wchar_t **stringPtr, _Out_ size_t *stringLength)
{
    VALIDATE_JSREF(stringValue);
    PARAM_NOT_NULL(stringPtr);
    *stringPtr = nullptr;
    PARAM_NOT_NULL(stringLength);
    *stringLength = 0;

    if (!Js::JavascriptString::Is(stringValue))
    {
        return JsErrorInvalidArgument;
    }

    return GlobalAPIWrapper([&]() -> JsErrorCode {
        Js::JavascriptString *jsString = Js::JavascriptString::FromVar(stringValue);

        *stringPtr = jsString->GetSz();
        *stringLength = jsString->GetLength();
        return JsNoError;
    });
}

CHAKRA_API JsStringToPointerUtf8(_In_ JsValueRef stringValue, _Outptr_result_buffer_(*stringLength) const char **stringPtr, _Out_ size_t *stringLength)
{
    const wchar_t* wstr;
    size_t wstrLen;
    JsErrorCode err = JsStringToPointer(stringValue, &wstr, &wstrLen);
    if (err == JsNoError)
    {
        PARAM_NOT_NULL(stringPtr);
        PARAM_NOT_NULL(stringLength);
        *stringPtr = nullptr;
        *stringLength = 0;

        // xplat-todo: fix lifetime. caller allocate space, or caller free?
        // xplat-todo: fix encoding. The result is cesu8.
        utf8::WideToNarrow str(wstr, wstrLen);
        if (str)
        {
            *stringPtr = str.Detach();
            *stringLength = str.Length();
        }
        else
        {
            err = JsErrorOutOfMemory;
        }
    }

    return err;
}

CHAKRA_API JsConvertValueToString(_In_ JsValueRef value, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(value, scriptContext);
        PARAM_NOT_NULL(result);
        *result = nullptr;

        *result = (JsValueRef) Js::JavascriptConversion::ToString((Js::Var)value, scriptContext);
        return JsNoError;
    });
}

CHAKRA_API JsGetGlobalObject(_Out_ JsValueRef *globalObject)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(globalObject);

        *globalObject = (JsValueRef)scriptContext->GetGlobalObject();
        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

CHAKRA_API JsCreateObject(_Out_ JsValueRef *object)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(object);

        *object = scriptContext->GetLibrary()->CreateObject();
        return JsNoError;
    });
}

CHAKRA_API JsCreateExternalObject(_In_opt_ void *data, _In_opt_ JsFinalizeCallback finalizeCallback, _Out_ JsValueRef *object)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(object);

        *object = RecyclerNewFinalized(scriptContext->GetRecycler(), JsrtExternalObject, RecyclerNew(scriptContext->GetRecycler(), JsrtExternalType, scriptContext, finalizeCallback), data);
        return JsNoError;
    });
}

CHAKRA_API JsConvertValueToObject(_In_ JsValueRef value, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(value, scriptContext);
        PARAM_NOT_NULL(result);

        *result = (JsValueRef)Js::JavascriptOperators::ToObject((Js::Var)value, scriptContext);
        Assert(*result == nullptr || !Js::CrossSite::NeedMarshalVar(*result, scriptContext));

        return JsNoError;
    });
}

CHAKRA_API JsGetPrototype(_In_ JsValueRef object, _Out_ JsValueRef *prototypeObject)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        PARAM_NOT_NULL(prototypeObject);

        *prototypeObject = (JsValueRef)Js::JavascriptOperators::OP_GetPrototype(object, scriptContext);
        Assert(*prototypeObject == nullptr || !Js::CrossSite::NeedMarshalVar(*prototypeObject, scriptContext));

        return JsNoError;
    });
}

CHAKRA_API JsSetPrototype(_In_ JsValueRef object, _In_ JsValueRef prototypeObject)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_OBJECT_OR_NULL(prototypeObject, scriptContext);

        // We're not allowed to set this.
        if (object == scriptContext->GetLibrary()->GetObjectPrototype())
        {
            return JsErrorInvalidArgument;
        }

        Js::JavascriptObject::ChangePrototype(Js::RecyclableObject::FromVar(object), Js::RecyclableObject::FromVar(prototypeObject), true, scriptContext);

        return JsNoError;
    });
}

CHAKRA_API JsInstanceOf(_In_ JsValueRef object, _In_ JsValueRef constructor, _Out_ bool *result) {
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(object, scriptContext);
        VALIDATE_INCOMING_REFERENCE(constructor, scriptContext);
        PARAM_NOT_NULL(result);

        *result = Js::RecyclableObject::FromVar(constructor)->HasInstance(object, scriptContext) ? true : false;

        return JsNoError;
    });
}

CHAKRA_API JsGetExtensionAllowed(_In_ JsValueRef object, _Out_ bool *value)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        PARAM_NOT_NULL(value);
        *value = nullptr;

        *value = Js::RecyclableObject::FromVar(object)->IsExtensible() != 0;

        return JsNoError;
    });
}

CHAKRA_API JsPreventExtension(_In_ JsValueRef object)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);

        Js::RecyclableObject::FromVar(object)->PreventExtensions();

        return JsNoError;
    });
}

CHAKRA_API JsGetProperty(_In_ JsValueRef object, _In_ JsPropertyIdRef propertyId, _Out_ JsValueRef *value)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        PARAM_NOT_NULL(value);
        *value = nullptr;

        *value = Js::JavascriptOperators::OP_GetProperty((Js::Var)object, ((Js::PropertyRecord *)propertyId)->GetPropertyId(), scriptContext);
        Assert(*value == nullptr || !Js::CrossSite::NeedMarshalVar(*value, scriptContext));
        return JsNoError;
    });
}

CHAKRA_API JsGetOwnPropertyDescriptor(_In_ JsValueRef object, _In_ JsPropertyIdRef propertyId, _Out_ JsValueRef *propertyDescriptor)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        PARAM_NOT_NULL(propertyDescriptor);
        *propertyDescriptor = nullptr;

        Js::PropertyDescriptor propertyDescriptorValue;
        if (Js::JavascriptOperators::GetOwnPropertyDescriptor(Js::RecyclableObject::FromVar(object), ((Js::PropertyRecord *)propertyId)->GetPropertyId(), scriptContext, &propertyDescriptorValue))
        {
            *propertyDescriptor = Js::JavascriptOperators::FromPropertyDescriptor(propertyDescriptorValue, scriptContext);
        }
        else
        {
            *propertyDescriptor = scriptContext->GetLibrary()->GetUndefined();
        }
        Assert(*propertyDescriptor == nullptr || !Js::CrossSite::NeedMarshalVar(*propertyDescriptor, scriptContext));
        return JsNoError;
    });
}

CHAKRA_API JsGetOwnPropertyNames(_In_ JsValueRef object, _Out_ JsValueRef *propertyNames)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        PARAM_NOT_NULL(propertyNames);
        *propertyNames = nullptr;

        *propertyNames = Js::JavascriptOperators::GetOwnPropertyNames(object, scriptContext);
        Assert(*propertyNames == nullptr || !Js::CrossSite::NeedMarshalVar(*propertyNames, scriptContext));
        return JsNoError;
    });
}

CHAKRA_API JsGetOwnPropertySymbols(_In_ JsValueRef object, _Out_ JsValueRef *propertySymbols)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        PARAM_NOT_NULL(propertySymbols);

        *propertySymbols = Js::JavascriptOperators::GetOwnPropertySymbols(object, scriptContext);
        Assert(*propertySymbols == nullptr || !Js::CrossSite::NeedMarshalVar(*propertySymbols, scriptContext));
        return JsNoError;
    });
}

CHAKRA_API JsSetProperty(_In_ JsValueRef object, _In_ JsPropertyIdRef propertyId, _In_ JsValueRef value, _In_ bool useStrictRules)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        VALIDATE_INCOMING_REFERENCE(value, scriptContext);

        Js::JavascriptOperators::OP_SetProperty(object, ((Js::PropertyRecord *)propertyId)->GetPropertyId(), value, scriptContext,
            nullptr, useStrictRules ? Js::PropertyOperation_StrictMode : Js::PropertyOperation_None);

        return JsNoError;
    });
}

CHAKRA_API JsHasProperty(_In_ JsValueRef object, _In_ JsPropertyIdRef propertyId, _Out_ bool *hasProperty)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        PARAM_NOT_NULL(hasProperty);
        *hasProperty = nullptr;

        *hasProperty = Js::JavascriptOperators::OP_HasProperty(object, ((Js::PropertyRecord *)propertyId)->GetPropertyId(), scriptContext) != 0;

        return JsNoError;
    });
}

CHAKRA_API JsDeleteProperty(_In_ JsValueRef object, _In_ JsPropertyIdRef propertyId, _In_ bool useStrictRules, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        PARAM_NOT_NULL(result);
        *result = nullptr;

        *result = Js::JavascriptOperators::OP_DeleteProperty((Js::Var)object, ((Js::PropertyRecord *)propertyId)->GetPropertyId(),
            scriptContext, useStrictRules ? Js::PropertyOperation_StrictMode : Js::PropertyOperation_None);
        Assert(*result == nullptr || !Js::CrossSite::NeedMarshalVar(*result, scriptContext));
        return JsNoError;
    });
}

CHAKRA_API JsDefineProperty(_In_ JsValueRef object, _In_ JsPropertyIdRef propertyId, _In_ JsValueRef propertyDescriptor, _Out_ bool *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        VALIDATE_INCOMING_OBJECT(propertyDescriptor, scriptContext);
        PARAM_NOT_NULL(result);
        *result = nullptr;

        Js::PropertyDescriptor propertyDescriptorValue;
        if (!Js::JavascriptOperators::ToPropertyDescriptor(propertyDescriptor, &propertyDescriptorValue, scriptContext))
        {
            return JsErrorInvalidArgument;
        }

        *result = Js::JavascriptOperators::DefineOwnPropertyDescriptor(
            Js::RecyclableObject::FromVar(object), ((Js::PropertyRecord *)propertyId)->GetPropertyId(), propertyDescriptorValue,
            true, scriptContext) != 0;

        return JsNoError;
    });
}

CHAKRA_API JsCreateArray(_In_ unsigned int length, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(result);
        *result = nullptr;

        *result = scriptContext->GetLibrary()->CreateArray(length);

        return JsNoError;
    });
}

CHAKRA_API JsCreateArrayBuffer(_In_ unsigned int byteLength, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(result);

        Js::JavascriptLibrary* library = scriptContext->GetLibrary();
        *result = library->CreateArrayBuffer(byteLength);

        JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(*result));
        return JsNoError;
    });
}

CHAKRA_API JsCreateExternalArrayBuffer(_Pre_maybenull_ _Pre_writable_byte_size_(byteLength) void *data, _In_ unsigned int byteLength,
    _In_opt_ JsFinalizeCallback finalizeCallback, _In_opt_ void *callbackState, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(result);

        if (data == nullptr && byteLength > 0)
        {
            return JsErrorInvalidArgument;
        }

        Js::JavascriptLibrary* library = scriptContext->GetLibrary();
        *result = Js::JsrtExternalArrayBuffer::New(
            reinterpret_cast<BYTE*>(data),
            byteLength,
            finalizeCallback,
            callbackState,
            library->GetArrayBufferType());

        JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(*result));
        return JsNoError;
    });
}

CHAKRA_API JsCreateTypedArray(_In_ JsTypedArrayType arrayType, _In_ JsValueRef baseArray, _In_ unsigned int byteOffset,
    _In_ unsigned int elementLength, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        if (baseArray != JS_INVALID_REFERENCE)
        {
            VALIDATE_INCOMING_REFERENCE(baseArray, scriptContext);
        }
        PARAM_NOT_NULL(result);

        Js::JavascriptLibrary* library = scriptContext->GetLibrary();

        const bool fromArrayBuffer = (baseArray != JS_INVALID_REFERENCE && Js::ArrayBuffer::Is(baseArray));

        if (byteOffset != 0 && !fromArrayBuffer)
        {
            return JsErrorInvalidArgument;
        }

        if (elementLength != 0 && !(baseArray == JS_INVALID_REFERENCE || fromArrayBuffer))
        {
            return JsErrorInvalidArgument;
        }

        Js::JavascriptFunction* constructorFunc = nullptr;
        Js::Var values[4] =
        {
            library->GetUndefined(),
            baseArray != nullptr ? baseArray : Js::JavascriptNumber::ToVar(elementLength, scriptContext)
        };
        if (fromArrayBuffer)
        {
            values[2] = Js::JavascriptNumber::ToVar(byteOffset, scriptContext);
            values[3] = Js::JavascriptNumber::ToVar(elementLength, scriptContext);
        }

        Js::CallInfo info(Js::CallFlags_New, fromArrayBuffer ? 4 : 2);
        Js::Arguments args(info, values);

        switch (arrayType)
        {
        case JsArrayTypeInt8:
            constructorFunc = library->GetInt8ArrayConstructor();
            break;
        case JsArrayTypeUint8:
            constructorFunc = library->GetUint8ArrayConstructor();
            break;
        case JsArrayTypeUint8Clamped:
            constructorFunc = library->GetUint8ClampedArrayConstructor();
            break;
        case JsArrayTypeInt16:
            constructorFunc = library->GetInt16ArrayConstructor();
            break;
        case JsArrayTypeUint16:
            constructorFunc = library->GetUint16ArrayConstructor();
            break;
        case JsArrayTypeInt32:
            constructorFunc = library->GetInt32ArrayConstructor();
            break;
        case JsArrayTypeUint32:
            constructorFunc = library->GetUint32ArrayConstructor();
            break;
        case JsArrayTypeFloat32:
            constructorFunc = library->GetFloat32ArrayConstructor();
            break;
        case JsArrayTypeFloat64:
            constructorFunc = library->GetFloat64ArrayConstructor();
            break;
        default:
            return JsErrorInvalidArgument;
        }

        *result = Js::JavascriptFunction::CallAsConstructor(constructorFunc, /* overridingNewTarget = */nullptr, args, scriptContext);

        JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(*result));
        return JsNoError;
    });
}

CHAKRA_API JsCreateDataView(_In_ JsValueRef arrayBuffer, _In_ unsigned int byteOffset, _In_ unsigned int byteLength, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(arrayBuffer, scriptContext);
        PARAM_NOT_NULL(result);

        if (!Js::ArrayBuffer::Is(arrayBuffer))
        {
            return JsErrorInvalidArgument;
        }

        Js::JavascriptLibrary* library = scriptContext->GetLibrary();
        *result = library->CreateDataView(Js::ArrayBuffer::FromVar(arrayBuffer), byteOffset, byteLength);

        JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(*result));
        return JsNoError;
    });
}


C_ASSERT(JsArrayTypeUint8         - Js::TypeIds_Uint8Array        == JsArrayTypeInt8 - Js::TypeIds_Int8Array);
C_ASSERT(JsArrayTypeUint8Clamped  - Js::TypeIds_Uint8ClampedArray == JsArrayTypeInt8 - Js::TypeIds_Int8Array);
C_ASSERT(JsArrayTypeInt16         - Js::TypeIds_Int16Array        == JsArrayTypeInt8 - Js::TypeIds_Int8Array);
C_ASSERT(JsArrayTypeUint16        - Js::TypeIds_Uint16Array       == JsArrayTypeInt8 - Js::TypeIds_Int8Array);
C_ASSERT(JsArrayTypeInt32         - Js::TypeIds_Int32Array        == JsArrayTypeInt8 - Js::TypeIds_Int8Array);
C_ASSERT(JsArrayTypeUint32        - Js::TypeIds_Uint32Array       == JsArrayTypeInt8 - Js::TypeIds_Int8Array);
C_ASSERT(JsArrayTypeFloat32       - Js::TypeIds_Float32Array      == JsArrayTypeInt8 - Js::TypeIds_Int8Array);
C_ASSERT(JsArrayTypeFloat64       - Js::TypeIds_Float64Array      == JsArrayTypeInt8 - Js::TypeIds_Int8Array);

inline JsTypedArrayType GetTypedArrayType(Js::TypeId typeId)
{
    Assert(Js::TypedArrayBase::Is(typeId));
    return static_cast<JsTypedArrayType>(typeId + (JsArrayTypeInt8 - Js::TypeIds_Int8Array));
}

CHAKRA_API JsGetTypedArrayInfo(_In_ JsValueRef typedArray, _Out_opt_ JsTypedArrayType *arrayType, _Out_opt_ JsValueRef *arrayBuffer,
    _Out_opt_ unsigned int *byteOffset, _Out_opt_ unsigned int *byteLength)
{
    VALIDATE_JSREF(typedArray);

    BEGIN_JSRT_NO_EXCEPTION
    {
        const Js::TypeId typeId = Js::JavascriptOperators::GetTypeId(typedArray);

        if (!Js::TypedArrayBase::Is(typeId))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        if (arrayType != nullptr) {
            *arrayType = GetTypedArrayType(typeId);
        }

        Js::TypedArrayBase* typedArrayBase = Js::TypedArrayBase::FromVar(typedArray);
        if (arrayBuffer != nullptr) {
            *arrayBuffer = typedArrayBase->GetArrayBuffer();
        }

        if (byteOffset != nullptr) {
            *byteOffset = typedArrayBase->GetByteOffset();
        }

        if (byteLength != nullptr) {
            *byteLength = typedArrayBase->GetByteLength();
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsGetArrayBufferStorage(_In_ JsValueRef instance, _Outptr_result_bytebuffer_(*bufferLength) BYTE **buffer,
    _Out_ unsigned int *bufferLength)
{
    VALIDATE_JSREF(instance);
    PARAM_NOT_NULL(buffer);
    PARAM_NOT_NULL(bufferLength);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (!Js::ArrayBuffer::Is(instance))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        Js::ArrayBuffer* arrayBuffer = Js::ArrayBuffer::FromVar(instance);
        *buffer = arrayBuffer->GetBuffer();
        *bufferLength = arrayBuffer->GetByteLength();
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsGetTypedArrayStorage(_In_ JsValueRef instance, _Outptr_result_bytebuffer_(*bufferLength) BYTE **buffer,
    _Out_ unsigned int *bufferLength, _Out_opt_ JsTypedArrayType *typedArrayType, _Out_opt_ int *elementSize)
{
    VALIDATE_JSREF(instance);
    PARAM_NOT_NULL(buffer);
    PARAM_NOT_NULL(bufferLength);

    BEGIN_JSRT_NO_EXCEPTION
    {
        const Js::TypeId typeId = Js::JavascriptOperators::GetTypeId(instance);
        if (!Js::TypedArrayBase::Is(typeId))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        Js::TypedArrayBase* typedArrayBase = Js::TypedArrayBase::FromVar(instance);
        *buffer = typedArrayBase->GetByteBuffer();
        *bufferLength = typedArrayBase->GetByteLength();

        if (typedArrayType)
        {
            *typedArrayType = GetTypedArrayType(typeId);
        }

        if (elementSize)
        {
            switch (typeId)
            {
                case Js::TypeIds_Int8Array:
                    *elementSize = sizeof(int8);
                    break;
                case Js::TypeIds_Uint8Array:
                    *elementSize = sizeof(uint8);
                    break;
                case Js::TypeIds_Uint8ClampedArray:
                    *elementSize = sizeof(uint8);
                    break;
                case Js::TypeIds_Int16Array:
                    *elementSize = sizeof(int16);
                    break;
                case Js::TypeIds_Uint16Array:
                    *elementSize = sizeof(uint16);
                    break;
                case Js::TypeIds_Int32Array:
                    *elementSize = sizeof(int32);
                    break;
                case Js::TypeIds_Uint32Array:
                    *elementSize = sizeof(uint32);
                    break;
                case Js::TypeIds_Float32Array:
                    *elementSize = sizeof(float);
                    break;
                case Js::TypeIds_Float64Array:
                    *elementSize = sizeof(double);
                    break;
                default:
                    AssertMsg(FALSE, "invalid typed array type");
                    *elementSize = 1;
                    RETURN_NO_EXCEPTION(JsErrorFatal);
            }
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsGetDataViewStorage(_In_ JsValueRef instance, _Outptr_result_bytebuffer_(*bufferLength) BYTE **buffer, _Out_ unsigned int *bufferLength)
{
    VALIDATE_JSREF(instance);
    PARAM_NOT_NULL(buffer);
    PARAM_NOT_NULL(bufferLength);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (!Js::DataView::Is(instance))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        Js::DataView* dataView = Js::DataView::FromVar(instance);
        *buffer = dataView->GetArrayBuffer()->GetBuffer() + dataView->GetByteOffset();
        *bufferLength = dataView->GetLength();
    }
    END_JSRT_NO_EXCEPTION
}


CHAKRA_API JsCreateSymbol(_In_ JsValueRef description, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(result);
        *result = nullptr;

        Js::JavascriptString* descriptionString;

        if (description != JS_INVALID_REFERENCE)
        {
            VALIDATE_INCOMING_REFERENCE(description, scriptContext);
            descriptionString = Js::JavascriptConversion::ToString(description, scriptContext);
        }
        else
        {
            descriptionString = scriptContext->GetLibrary()->GetEmptyString();
        }

        *result = scriptContext->GetLibrary()->CreateSymbol(descriptionString);

        return JsNoError;
    });
}

CHAKRA_API JsHasIndexedProperty(_In_ JsValueRef object, _In_ JsValueRef index, _Out_ bool *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_REFERENCE(index, scriptContext);
        PARAM_NOT_NULL(result);
        *result = false;

        *result = Js::JavascriptOperators::OP_HasItem((Js::Var)object, (Js::Var)index, scriptContext) != 0;

        return JsNoError;
    });
}

CHAKRA_API JsGetIndexedProperty(_In_ JsValueRef object, _In_ JsValueRef index, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_REFERENCE(index, scriptContext);
        PARAM_NOT_NULL(result);
        *result = nullptr;

        *result = (JsValueRef)Js::JavascriptOperators::OP_GetElementI((Js::Var)object, (Js::Var)index, scriptContext);

        return JsNoError;
    });
}

CHAKRA_API JsSetIndexedProperty(_In_ JsValueRef object, _In_ JsValueRef index, _In_ JsValueRef value)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_REFERENCE(index, scriptContext);
        VALIDATE_INCOMING_REFERENCE(value, scriptContext);

        Js::JavascriptOperators::OP_SetElementI((Js::Var)object, (Js::Var)index, (Js::Var)value, scriptContext);

        return JsNoError;
    });
}

CHAKRA_API JsDeleteIndexedProperty(_In_ JsValueRef object, _In_ JsValueRef index)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);
        VALIDATE_INCOMING_REFERENCE(index, scriptContext);

        Js::JavascriptOperators::OP_DeleteElementI((Js::Var)object, (Js::Var)index, scriptContext);

        return JsNoError;
    });
}

template <class T, bool clamped = false> struct TypedArrayTypeTraits { static const JsTypedArrayType cTypedArrayType; };
template<> struct TypedArrayTypeTraits<int8> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeInt8; };
template<> struct TypedArrayTypeTraits<uint8, false> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeUint8; };
template<> struct TypedArrayTypeTraits<uint8, true> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeUint8Clamped; };
template<> struct TypedArrayTypeTraits<int16> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeInt16; };
template<> struct TypedArrayTypeTraits<uint16> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeUint16; };
template<> struct TypedArrayTypeTraits<int32> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeInt32; };
template<> struct TypedArrayTypeTraits<uint32> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeUint32; };
template<> struct TypedArrayTypeTraits<float> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeFloat32; };
template<> struct TypedArrayTypeTraits<double> { static const JsTypedArrayType cTypedArrayType = JsTypedArrayType::JsArrayTypeFloat64; };

template <class T, bool clamped = false>
Js::ArrayObject* CreateTypedArray(Js::ScriptContext *scriptContext, void* data, unsigned int length)
{
    Js::JavascriptLibrary* library = scriptContext->GetLibrary();
    Js::ArrayBuffer* arrayBuffer = RecyclerNew(
        scriptContext->GetRecycler(),
        Js::ExternalArrayBuffer,
        reinterpret_cast<BYTE*>(data),
        length * sizeof(T),
        library->GetArrayBufferType());

    return static_cast<Js::ArrayObject*>(Js::TypedArray<T, clamped>::Create(arrayBuffer, 0, length, library));
}

template <class T, bool clamped = false>
void GetObjectArrayData(Js::ArrayObject* objectArray, void** data, JsTypedArrayType* arrayType, uint* length)
{
    Js::TypedArray<T, clamped>* typedArray = Js::TypedArray<T, clamped>::FromVar(objectArray);
    *data = typedArray->GetArrayBuffer()->GetBuffer();
    *arrayType = TypedArrayTypeTraits<T, clamped>::cTypedArrayType;
    *length = typedArray->GetLength();
}

CHAKRA_API JsSetIndexedPropertiesToExternalData(
    _In_ JsValueRef object,
    _In_ void* data,
    _In_ JsTypedArrayType arrayType,
    _In_ unsigned int elementLength)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_OBJECT(object, scriptContext);

        // Don't support doing this on array or array-like object
        Js::TypeId typeId = Js::JavascriptOperators::GetTypeId(object);
        if (!Js::DynamicType::Is(typeId)
            || Js::DynamicObject::IsAnyArrayTypeId(typeId)
            || (typeId >= Js::TypeIds_TypedArrayMin && typeId <= Js::TypeIds_TypedArrayMax)
            || typeId == Js::TypeIds_ArrayBuffer
            || typeId == Js::TypeIds_DataView
            || Js::RecyclableObject::FromVar(object)->IsExternal()
            )
        {
            return JsErrorInvalidArgument;
        }

        if (data == nullptr && elementLength > 0)
        {
            return JsErrorInvalidArgument;
        }

        Js::ArrayObject* newTypedArray = nullptr;
        switch (arrayType)
        {
        case JsArrayTypeInt8:
            newTypedArray = CreateTypedArray<int8>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeUint8:
            newTypedArray = CreateTypedArray<uint8>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeUint8Clamped:
            newTypedArray = CreateTypedArray<uint8, true>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeInt16:
            newTypedArray = CreateTypedArray<int16>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeUint16:
            newTypedArray = CreateTypedArray<uint16>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeInt32:
            newTypedArray = CreateTypedArray<int32>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeUint32:
            newTypedArray = CreateTypedArray<uint32>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeFloat32:
            newTypedArray = CreateTypedArray<float>(scriptContext, data, elementLength);
            break;
        case JsArrayTypeFloat64:
            newTypedArray = CreateTypedArray<double>(scriptContext, data, elementLength);
            break;
        default:
            return JsErrorInvalidArgument;
        }

        Js::DynamicObject* dynamicObject = Js::DynamicObject::FromVar(object);
        dynamicObject->SetObjectArray(newTypedArray);

        return JsNoError;
    });
}

CHAKRA_API JsHasIndexedPropertiesExternalData(_In_ JsValueRef object, _Out_ bool *value)
{
    VALIDATE_JSREF(object);
    PARAM_NOT_NULL(value);

    BEGIN_JSRT_NO_EXCEPTION
    {
        *value = false;

        if (Js::DynamicType::Is(Js::JavascriptOperators::GetTypeId(object)))
        {
            Js::DynamicObject* dynamicObject = Js::DynamicObject::FromVar(object);
            Js::ArrayObject* objectArray = dynamicObject->GetObjectArray();
            *value = (objectArray && !Js::DynamicObject::IsAnyArray(objectArray));
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsGetIndexedPropertiesExternalData(
    _In_ JsValueRef object,
    _Out_ void** buffer,
    _Out_ JsTypedArrayType* arrayType,
    _Out_ unsigned int* elementLength)
{
    VALIDATE_JSREF(object);
    PARAM_NOT_NULL(buffer);
    PARAM_NOT_NULL(arrayType);
    PARAM_NOT_NULL(elementLength);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (!Js::DynamicType::Is(Js::JavascriptOperators::GetTypeId(object)))
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        *buffer = nullptr;
        *arrayType = JsTypedArrayType();
        *elementLength = 0;

        Js::DynamicObject* dynamicObject = Js::DynamicObject::FromVar(object);
        Js::ArrayObject* objectArray = dynamicObject->GetObjectArray();
        if (!objectArray)
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }

        switch (Js::JavascriptOperators::GetTypeId(objectArray))
        {
        case Js::TypeIds_Int8Array:
            GetObjectArrayData<int8>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Uint8Array:
            GetObjectArrayData<uint8>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Uint8ClampedArray:
            GetObjectArrayData<uint8, true>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Int16Array:
            GetObjectArrayData<int16>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Uint16Array:
            GetObjectArrayData<uint16>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Int32Array:
            GetObjectArrayData<int32>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Uint32Array:
            GetObjectArrayData<uint32>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Float32Array:
            GetObjectArrayData<float>(objectArray, buffer, arrayType, elementLength);
            break;
        case Js::TypeIds_Float64Array:
            GetObjectArrayData<double>(objectArray, buffer, arrayType, elementLength);
            break;
        default:
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsEquals(_In_ JsValueRef object1, _In_ JsValueRef object2, _Out_ bool *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(object1, scriptContext);
        VALIDATE_INCOMING_REFERENCE(object2, scriptContext);
        PARAM_NOT_NULL(result);

        *result = Js::JavascriptOperators::Equal((Js::Var)object1, (Js::Var)object2, scriptContext) != 0;
        return JsNoError;
    });
}

CHAKRA_API JsStrictEquals(_In_ JsValueRef object1, _In_ JsValueRef object2, _Out_ bool *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(object1, scriptContext);
        VALIDATE_INCOMING_REFERENCE(object2, scriptContext);
        PARAM_NOT_NULL(result);

        *result = Js::JavascriptOperators::StrictEqual((Js::Var)object1, (Js::Var)object2, scriptContext) != 0;
        return JsNoError;
    });
}

CHAKRA_API JsHasExternalData(_In_ JsValueRef object, _Out_ bool *value)
{
    VALIDATE_JSREF(object);
    PARAM_NOT_NULL(value);

    BEGIN_JSRT_NO_EXCEPTION
    {
        *value = JsrtExternalObject::Is(object);
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsGetExternalData(_In_ JsValueRef object, _Out_ void **data)
{
    VALIDATE_JSREF(object);
    PARAM_NOT_NULL(data);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (JsrtExternalObject::Is(object))
        {
            *data = JsrtExternalObject::FromVar(object)->GetSlotData();
        }
        else
        {
            *data = nullptr;
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsSetExternalData(_In_ JsValueRef object, _In_opt_ void *data)
{
    VALIDATE_JSREF(object);

    BEGIN_JSRT_NO_EXCEPTION
    {
        if (JsrtExternalObject::Is(object))
        {
            JsrtExternalObject::FromVar(object)->SetSlotData(data);
        }
        else
        {
            RETURN_NO_EXCEPTION(JsErrorInvalidArgument);
        }
    }
    END_JSRT_NO_EXCEPTION
}

CHAKRA_API JsCallFunction(_In_ JsValueRef function, _In_reads_(cargs) JsValueRef *args, _In_ ushort cargs, _Out_opt_ JsValueRef *result)
{
    if (result != nullptr)
    {
        *result = nullptr;
    }

    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_FUNCTION(function, scriptContext);

        if (cargs == 0 || args == nullptr) {
          return JsErrorInvalidArgument;
        }

        for (int index = 0; index < cargs; index++)
        {
            VALIDATE_INCOMING_REFERENCE(args[index], scriptContext);
        }

        Js::JavascriptFunction *jsFunction = Js::JavascriptFunction::FromVar(function);
        Js::CallInfo callInfo(cargs);
        Js::Arguments jsArgs(callInfo, reinterpret_cast<Js::Var *>(args));

        Js::Var varResult = jsFunction->CallRootFunction(jsArgs, scriptContext, true);
        if (result != nullptr)
        {
            *result = varResult;
            Assert(*result == nullptr || !Js::CrossSite::NeedMarshalVar(*result, scriptContext));
        }

        return JsNoError;
    });
}

CHAKRA_API JsConstructObject(_In_ JsValueRef function, _In_reads_(cargs) JsValueRef *args, _In_ ushort cargs, _Out_ JsValueRef *result)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_FUNCTION(function, scriptContext);
        PARAM_NOT_NULL(result);
        *result = nullptr;

        if (cargs == 0 || args == nullptr)
        {
            return JsErrorInvalidArgument;
        }

        for (int index = 0; index < cargs; index++)
        {
            VALIDATE_INCOMING_REFERENCE(args[index], scriptContext);
        }

        Js::JavascriptFunction *jsFunction = Js::JavascriptFunction::FromVar(function);
        Js::CallInfo callInfo(Js::CallFlags::CallFlags_New, cargs);
        Js::Arguments jsArgs(callInfo, reinterpret_cast<Js::Var *>(args));

        *result = Js::JavascriptFunction::CallAsConstructor(jsFunction, /* overridingNewTarget = */nullptr, jsArgs, scriptContext);
        Assert(*result == nullptr || !Js::CrossSite::NeedMarshalVar(*result, scriptContext));
        return JsNoError;
    });
}

CHAKRA_API JsCreateFunction(_In_ JsNativeFunction nativeFunction, _In_opt_ void *callbackState, _Out_ JsValueRef *function)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(nativeFunction);
        PARAM_NOT_NULL(function);
        *function = nullptr;

        Js::JavascriptExternalFunction *externalFunction = scriptContext->GetLibrary()->CreateStdCallExternalFunction((Js::StdCallJavascriptMethod)nativeFunction, 0, callbackState);
        *function = (JsValueRef)externalFunction;

        return JsNoError;
    });
}

CHAKRA_API JsCreateNamedFunction(_In_ JsValueRef name, _In_ JsNativeFunction nativeFunction, _In_opt_ void *callbackState, _Out_ JsValueRef *function)
{
    return ContextAPIWrapper<true>([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(name, scriptContext);
        PARAM_NOT_NULL(nativeFunction);
        PARAM_NOT_NULL(function);
        *function = nullptr;

        if (name != JS_INVALID_REFERENCE)
        {
            name = Js::JavascriptConversion::ToString(name, scriptContext);
        }
        else
        {
            name = scriptContext->GetLibrary()->GetEmptyString();
        }

        Js::JavascriptExternalFunction *externalFunction = scriptContext->GetLibrary()->CreateStdCallExternalFunction((Js::StdCallJavascriptMethod)nativeFunction, Js::JavascriptString::FromVar(name), callbackState);

        *function = (JsValueRef)externalFunction;
        return JsNoError;
    });
}

void SetErrorMessage(Js::ScriptContext *scriptContext, JsValueRef newError, JsValueRef message)
{
    Js::JavascriptOperators::OP_SetProperty(newError, Js::PropertyIds::message, message, scriptContext);
}

CHAKRA_API JsCreateError(_In_ JsValueRef message, _Out_ JsValueRef *error)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(message, scriptContext);
        PARAM_NOT_NULL(error);
        *error = nullptr;

        JsValueRef newError = scriptContext->GetLibrary()->CreateError();
        SetErrorMessage(scriptContext, newError, message);
        *error = newError;

        return JsNoError;
    });
}

CHAKRA_API JsCreateRangeError(_In_ JsValueRef message, _Out_ JsValueRef *error)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(message, scriptContext);
        PARAM_NOT_NULL(error);
        *error = nullptr;

        JsValueRef newError;

        newError = scriptContext->GetLibrary()->CreateRangeError();
        SetErrorMessage(scriptContext, newError, message);
        *error = newError;

        return JsNoError;
    });
}

CHAKRA_API JsCreateReferenceError(_In_ JsValueRef message, _Out_ JsValueRef *error)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(message, scriptContext);
        PARAM_NOT_NULL(error);
        *error = nullptr;

        JsValueRef newError;

        newError = scriptContext->GetLibrary()->CreateReferenceError();
        SetErrorMessage(scriptContext, newError, message);
        *error = newError;

        return JsNoError;
    });
}

CHAKRA_API JsCreateSyntaxError(_In_ JsValueRef message, _Out_ JsValueRef *error)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(message, scriptContext);
        PARAM_NOT_NULL(error);
        *error = nullptr;

        JsValueRef newError;

        newError = scriptContext->GetLibrary()->CreateSyntaxError();
        SetErrorMessage(scriptContext, newError, message);
        *error = newError;

        return JsNoError;
    });
}

CHAKRA_API JsCreateTypeError(_In_ JsValueRef message, _Out_ JsValueRef *error)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(message, scriptContext);
        PARAM_NOT_NULL(error);
        *error = nullptr;

        JsValueRef newError;

        newError = scriptContext->GetLibrary()->CreateTypeError();
        SetErrorMessage(scriptContext, newError, message);
        *error = newError;

        return JsNoError;
    });
}

CHAKRA_API JsCreateURIError(_In_ JsValueRef message, _Out_ JsValueRef *error)
{
    return ContextAPIWrapper<true>([&] (Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(message, scriptContext);
        PARAM_NOT_NULL(error);
        *error = nullptr;

        JsValueRef newError;

        newError = scriptContext->GetLibrary()->CreateURIError();
        SetErrorMessage(scriptContext, newError, message);
        *error = newError;

        return JsNoError;
    });
}

CHAKRA_API JsHasException(_Out_ bool *hasException)
{
    PARAM_NOT_NULL(hasException);
    *hasException = false;

    JsrtContext *currentContext = JsrtContext::GetCurrent();

    if (currentContext == nullptr)
    {
        return JsErrorNoCurrentContext;
    }

    Js::ScriptContext *scriptContext = currentContext->GetScriptContext();
    Assert(scriptContext != nullptr);

    if (scriptContext->GetRecycler() && scriptContext->GetRecycler()->IsHeapEnumInProgress())
    {
        return JsErrorHeapEnumInProgress;
    }
    else if (scriptContext->GetThreadContext()->IsInThreadServiceCallback())
    {
        return JsErrorInThreadServiceCallback;
    }

    if (scriptContext->GetThreadContext()->IsExecutionDisabled())
    {
        return JsErrorInDisabledState;
    }

    *hasException = scriptContext->HasRecordedException();

    return JsNoError;
}

CHAKRA_API JsGetAndClearException(_Out_ JsValueRef *exception)
{
    PARAM_NOT_NULL(exception);
    *exception = nullptr;

    JsrtContext *currentContext = JsrtContext::GetCurrent();

    if (currentContext == nullptr)
    {
        return JsErrorNoCurrentContext;
    }

    Js::ScriptContext *scriptContext = currentContext->GetScriptContext();
    Assert(scriptContext != nullptr);

    if (scriptContext->GetRecycler() && scriptContext->GetRecycler()->IsHeapEnumInProgress())
    {
        return JsErrorHeapEnumInProgress;
    }
    else if (scriptContext->GetThreadContext()->IsInThreadServiceCallback())
    {
        return JsErrorInThreadServiceCallback;
    }

    if (scriptContext->GetThreadContext()->IsExecutionDisabled())
    {
        return JsErrorInDisabledState;
    }

    HRESULT hr = S_OK;
    Js::JavascriptExceptionObject *recordedException = nullptr;

    BEGIN_TRANSLATE_OOM_TO_HRESULT
      recordedException = scriptContext->GetAndClearRecordedException();
    END_TRANSLATE_OOM_TO_HRESULT(hr)

    if (hr == E_OUTOFMEMORY)
    {
      recordedException = scriptContext->GetThreadContext()->GetRecordedException();
    }
    if (recordedException == nullptr)
    {
        return JsErrorInvalidArgument;
    }

    *exception = recordedException->GetThrownObject(nullptr);
    if (*exception == nullptr)
    {
        return JsErrorInvalidArgument;
    }
    return JsNoError;
}

CHAKRA_API JsSetException(_In_ JsValueRef exception)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext* scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(exception, scriptContext);

        Js::JavascriptExceptionObject *exceptionObject;

        exceptionObject = RecyclerNew(scriptContext->GetRecycler(), Js::JavascriptExceptionObject, exception, scriptContext, nullptr);

        JsrtContext * context = JsrtContext::GetCurrent();
        JsrtRuntime * runtime = context->GetRuntime();

        scriptContext->RecordException(exceptionObject, runtime->DispatchExceptions());

        return JsNoError;
    });
}

CHAKRA_API JsGetRuntimeMemoryUsage(_In_ JsRuntimeHandle runtimeHandle, _Out_ size_t * memoryUsage)
{
    VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);
    PARAM_NOT_NULL(memoryUsage);
    *memoryUsage = 0;

    ThreadContext * threadContext = JsrtRuntime::FromHandle(runtimeHandle)->GetThreadContext();
    AllocationPolicyManager * allocPolicyManager = threadContext->GetAllocationPolicyManager();

    *memoryUsage = allocPolicyManager->GetUsage();

    return JsNoError;
}

CHAKRA_API JsSetRuntimeMemoryLimit(_In_ JsRuntimeHandle runtimeHandle, _In_ size_t memoryLimit)
{
    VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);

    ThreadContext * threadContext = JsrtRuntime::FromHandle(runtimeHandle)->GetThreadContext();
    AllocationPolicyManager * allocPolicyManager = threadContext->GetAllocationPolicyManager();

    allocPolicyManager->SetLimit(memoryLimit);

    return JsNoError;
}

CHAKRA_API JsGetRuntimeMemoryLimit(_In_ JsRuntimeHandle runtimeHandle, _Out_ size_t * memoryLimit)
{
    VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);
    PARAM_NOT_NULL(memoryLimit);
    *memoryLimit = 0;

    ThreadContext * threadContext = JsrtRuntime::FromHandle(runtimeHandle)->GetThreadContext();
    AllocationPolicyManager * allocPolicyManager = threadContext->GetAllocationPolicyManager();

    *memoryLimit = allocPolicyManager->GetLimit();

    return JsNoError;
}

C_ASSERT(JsMemoryAllocate == (_JsMemoryEventType) AllocationPolicyManager::MemoryAllocateEvent::MemoryAllocate);
C_ASSERT(JsMemoryFree == (_JsMemoryEventType) AllocationPolicyManager::MemoryAllocateEvent::MemoryFree);
C_ASSERT(JsMemoryFailure == (_JsMemoryEventType) AllocationPolicyManager::MemoryAllocateEvent::MemoryFailure);
C_ASSERT(JsMemoryFailure == (_JsMemoryEventType) AllocationPolicyManager::MemoryAllocateEvent::MemoryMax);

CHAKRA_API JsSetRuntimeMemoryAllocationCallback(_In_ JsRuntimeHandle runtime, _In_opt_ void *callbackState, _In_ JsMemoryAllocationCallback allocationCallback)
{
    VALIDATE_INCOMING_RUNTIME_HANDLE(runtime);

    ThreadContext* threadContext = JsrtRuntime::FromHandle(runtime)->GetThreadContext();
    AllocationPolicyManager * allocPolicyManager = threadContext->GetAllocationPolicyManager();

    allocPolicyManager->SetMemoryAllocationCallback(callbackState, (AllocationPolicyManager::PageAllocatorMemoryAllocationCallback)allocationCallback);

    return JsNoError;
}

CHAKRA_API JsSetRuntimeBeforeCollectCallback(_In_ JsRuntimeHandle runtime, _In_opt_ void *callbackState, _In_ JsBeforeCollectCallback beforeCollectCallback)
{
    return GlobalAPIWrapper([&]() -> JsErrorCode {
        VALIDATE_INCOMING_RUNTIME_HANDLE(runtime);

        JsrtRuntime::FromHandle(runtime)->SetBeforeCollectCallback(beforeCollectCallback, callbackState);
        return JsNoError;
    });
}

CHAKRA_API JsDisableRuntimeExecution(_In_ JsRuntimeHandle runtimeHandle)
{
    VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);

    ThreadContext * threadContext = JsrtRuntime::FromHandle(runtimeHandle)->GetThreadContext();
    if (!threadContext->TestThreadContextFlag(ThreadContextFlagCanDisableExecution))
    {
        return JsErrorCannotDisableExecution;
    }

    if (threadContext->GetRecycler() && threadContext->GetRecycler()->IsHeapEnumInProgress())
    {
        return JsErrorHeapEnumInProgress;
    }
    else if (threadContext->IsInThreadServiceCallback())
    {
        return JsErrorInThreadServiceCallback;
    }

    threadContext->DisableExecution();
    return JsNoError;
}

CHAKRA_API JsEnableRuntimeExecution(_In_ JsRuntimeHandle runtimeHandle)
{
    return GlobalAPIWrapper([&] () -> JsErrorCode {
        VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);

        ThreadContext * threadContext = JsrtRuntime::FromHandle(runtimeHandle)->GetThreadContext();
        if (!threadContext->TestThreadContextFlag(ThreadContextFlagCanDisableExecution))
        {
            return JsNoError;
        }

        if (threadContext->GetRecycler() && threadContext->GetRecycler()->IsHeapEnumInProgress())
        {
            return JsErrorHeapEnumInProgress;
        }
        else if (threadContext->IsInThreadServiceCallback())
        {
            return JsErrorInThreadServiceCallback;
        }

        ThreadContextScope scope(threadContext);

        if (!scope.IsValid())
        {
            return JsErrorWrongThread;
        }

        threadContext->EnableExecution();
        return JsNoError;
    });
}

CHAKRA_API JsIsRuntimeExecutionDisabled(_In_ JsRuntimeHandle runtimeHandle, _Out_ bool *isDisabled)
{
    VALIDATE_INCOMING_RUNTIME_HANDLE(runtimeHandle);
    PARAM_NOT_NULL(isDisabled);
    *isDisabled = false;

    ThreadContext* threadContext = JsrtRuntime::FromHandle(runtimeHandle)->GetThreadContext();
    *isDisabled = threadContext->IsExecutionDisabled();
    return JsNoError;
}

CHAKRA_API JsGetPropertyIdFromName(_In_z_ const wchar_t *name, _Out_ JsPropertyIdRef *propertyId)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext * scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(name);
        PARAM_NOT_NULL(propertyId);
        *propertyId = nullptr;

        size_t cPropertyNameLength = wcslen(name);

        if (cPropertyNameLength <= INT_MAX)
        {
            scriptContext->GetOrAddPropertyRecord(name, static_cast<int>(cPropertyNameLength), (Js::PropertyRecord const **)propertyId);

            return JsNoError;
        }
        else
        {
            return JsErrorOutOfMemory;
        }
    });
}

CHAKRA_API JsGetPropertyIdFromNameUtf8(_In_z_ const char *name, _Out_ JsPropertyIdRef *propertyId)
{
    // xplat-todo: should pass in utf8 length
    PARAM_NOT_NULL(name);
    utf8::NarrowToWide wname(name);
    if (!wname)
    {
      return JsErrorOutOfMemory;
    }

    // xplat-todo: does following accept embedded null?
    return JsGetPropertyIdFromName(wname, propertyId);
}

CHAKRA_API JsGetPropertyIdFromSymbol(_In_ JsValueRef symbol, _Out_ JsPropertyIdRef *propertyId)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_REFERENCE(symbol, scriptContext);
        PARAM_NOT_NULL(propertyId);
        *propertyId = nullptr;

        if (!Js::JavascriptSymbol::Is(symbol))
        {
            return JsErrorPropertyNotSymbol;
        }

        *propertyId = (JsPropertyIdRef)Js::JavascriptSymbol::FromVar(symbol)->GetValue();
        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

CHAKRA_API JsGetSymbolFromPropertyId(_In_ JsPropertyIdRef propertyId, _Out_ JsValueRef *symbol)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext * scriptContext) -> JsErrorCode {
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        PARAM_NOT_NULL(symbol);
        *symbol = nullptr;

        Js::PropertyRecord const * propertyRecord = (Js::PropertyRecord const *)propertyId;
        if (!propertyRecord->IsSymbol())
        {
            return JsErrorPropertyNotSymbol;
        }

        *symbol = scriptContext->GetLibrary()->CreateSymbol(propertyRecord);
        return JsNoError;
    });
}

#pragma prefast(suppress:6101, "Prefast doesn't see through the lambda")
CHAKRA_API JsGetPropertyNameFromId(_In_ JsPropertyIdRef propertyId, _Outptr_result_z_ const wchar_t **name)
{
    return GlobalAPIWrapper([&]() -> JsErrorCode {
        VALIDATE_INCOMING_PROPERTYID(propertyId);
        PARAM_NOT_NULL(name);
        *name = nullptr;

        Js::PropertyRecord const * propertyRecord = (Js::PropertyRecord const *)propertyId;

        if (propertyRecord->IsSymbol())
        {
            return JsErrorPropertyNotString;
        }

        *name = propertyRecord->GetBuffer();
        return JsNoError;
    });
}

CHAKRA_API JsGetPropertyIdType(_In_ JsPropertyIdRef propertyId, _Out_ JsPropertyIdType* propertyIdType)
{
    return GlobalAPIWrapper([&]() -> JsErrorCode {
        VALIDATE_INCOMING_PROPERTYID(propertyId);

        Js::PropertyRecord const * propertyRecord = (Js::PropertyRecord const *)propertyId;

        if (propertyRecord->IsSymbol())
        {
            *propertyIdType = JsPropertyIdTypeSymbol;
        }
        else
        {
            *propertyIdType = JsPropertyIdTypeString;
        }
        return JsNoError;
    });
}


CHAKRA_API JsGetRuntime(_In_ JsContextRef context, _Out_ JsRuntimeHandle *runtime)
{
    VALIDATE_JSREF(context);
    PARAM_NOT_NULL(runtime);

    *runtime = nullptr;

    if (!JsrtContext::Is(context))
    {
        return JsErrorInvalidArgument;
    }

    *runtime = static_cast<JsrtContext *>(context)->GetRuntime();
    return JsNoError;
}

CHAKRA_API JsIdle(_Out_opt_ unsigned int *nextIdleTick)
{
    PARAM_NOT_NULL(nextIdleTick);

    return ContextAPINoScriptWrapper(
        [&] (Js::ScriptContext * scriptContext) -> JsErrorCode {

            *nextIdleTick = 0;

            if (scriptContext->GetThreadContext()->GetRecycler() && scriptContext->GetThreadContext()->GetRecycler()->IsHeapEnumInProgress())
            {
                return JsErrorHeapEnumInProgress;
            }
            else if (scriptContext->GetThreadContext()->IsInThreadServiceCallback())
            {
                return JsErrorInThreadServiceCallback;
            }

            JsrtContext * context = JsrtContext::GetCurrent();
            JsrtRuntime * runtime = context->GetRuntime();

            if (!runtime->UseIdle())
            {
                return JsErrorIdleNotEnabled;
            }

            unsigned int ticks = runtime->Idle();

            *nextIdleTick = ticks;

            return JsNoError;
    });
}

CHAKRA_API JsSetPromiseContinuationCallback(_In_ JsPromiseContinuationCallback promiseContinuationCallback, _In_opt_ void *callbackState)
{
    return ContextAPINoScriptWrapper([&](Js::ScriptContext * scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(promiseContinuationCallback);

        scriptContext->GetLibrary()->SetNativeHostPromiseContinuationFunction((Js::JavascriptLibrary::PromiseContinuationCallback)promiseContinuationCallback, callbackState);
        return JsNoError;
    },
    /*allowInObjectBeforeCollectCallback*/true);
}

JsErrorCode RunScriptCore(const byte *script, size_t cb, LoadScriptFlag loadScriptFlag, JsSourceContext sourceContext, const wchar_t *sourceUrl, bool parseOnly, JsParseScriptAttributes parseAttributes, bool isSourceModule, JsValueRef *result)
{
    Js::JavascriptFunction *scriptFunction;
    CompileScriptException se;

    JsErrorCode errorCode = ContextAPINoScriptWrapper(
        [&](Js::ScriptContext * scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(script);
        PARAM_NOT_NULL(sourceUrl);


        SourceContextInfo * sourceContextInfo = scriptContext->GetSourceContextInfo(sourceContext, nullptr);

        if (sourceContextInfo == nullptr)
        {
            sourceContextInfo = scriptContext->CreateSourceContextInfo(sourceContext, sourceUrl, wcslen(sourceUrl), nullptr);
        }

        const int chsize = (loadScriptFlag & LoadScriptFlag_Utf8Source) ? sizeof(utf8char_t) : sizeof(wchar_t);
        SRCINFO si = {
            /* sourceContextInfo   */ sourceContextInfo,
            /* dlnHost             */ 0,
            /* ulColumnHost        */ 0,
            /* lnMinHost           */ 0,
            /* ichMinHost          */ 0,
            /* ichLimHost          */ static_cast<ULONG>(cb / chsize), // OK to truncate since this is used to limit sourceText in debugDocument/compilation errors.
            /* ulCharOffset        */ 0,
            /* mod                 */ kmodGlobal,
            /* grfsi               */ 0
        };

        Js::Utf8SourceInfo* utf8SourceInfo = nullptr;
        if (result != nullptr)
        {
            loadScriptFlag = (LoadScriptFlag)(loadScriptFlag | LoadScriptFlag_Expression);
        }
        bool isLibraryCode = (parseAttributes & JsParseScriptAttributeLibraryCode) == JsParseScriptAttributeLibraryCode;
        if (isLibraryCode)
        {
            loadScriptFlag = (LoadScriptFlag)(loadScriptFlag | LoadScriptFlag_LibraryCode);
        }
        if (isSourceModule)
        {
            loadScriptFlag = (LoadScriptFlag)(loadScriptFlag | LoadScriptFlag_Module);
        }
        scriptFunction = scriptContext->LoadScript(script, cb, &si, &se, &utf8SourceInfo, Js::Constants::GlobalCode, loadScriptFlag);

        JsrtContext * context = JsrtContext::GetCurrent();
        context->OnScriptLoad(scriptFunction, utf8SourceInfo, &se);

        return JsNoError;
    });

    if (errorCode != JsNoError)
    {
        return errorCode;
    }

    return ContextAPIWrapper<false>([&](Js::ScriptContext* scriptContext) -> JsErrorCode {
        if (scriptFunction == nullptr)
        {
            HandleScriptCompileError(scriptContext, &se);
            return JsErrorScriptCompile;
        }

        if (parseOnly)
        {
            PARAM_NOT_NULL(result);
            *result = scriptFunction;
        }
        else
        {
            Js::Arguments args(0, nullptr);
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
            Js::Var varThis;
            if (PHASE_FORCE1(Js::EvalCompilePhase))
            {
                varThis = Js::JavascriptOperators::OP_GetThis(scriptContext->GetLibrary()->GetUndefined(), kmodGlobal, scriptContext);
                args.Info.Flags = (Js::CallFlags)Js::CallFlags::CallFlags_Eval;
                args.Info.Count = 1;
                args.Values = &varThis;
            }
#endif
            Js::Var varResult = scriptFunction->CallRootFunction(args, scriptContext, true);
            if (result != nullptr)
            {
                *result = varResult;
            }
        }
        return JsNoError;
    });
}

JsErrorCode RunScriptCore(const char *script, JsSourceContext sourceContext, const char *sourceUrl, bool parseOnly, JsParseScriptAttributes parseAttributes, bool isSourceModule, JsValueRef *result)
{
    utf8::NarrowToWide url((LPCSTR)sourceUrl);
    if (!url)
    {
        return JsErrorOutOfMemory;
    }

    return RunScriptCore(reinterpret_cast<const byte*>(script), strlen(script), LoadScriptFlag_Utf8Source, sourceContext, url, parseOnly, parseAttributes, isSourceModule, result);
}

JsErrorCode RunScriptCore(const wchar_t *script, JsSourceContext sourceContext, const wchar_t *sourceUrl, bool parseOnly, JsParseScriptAttributes parseAttributes, bool isSourceModule, JsValueRef *result)
{
    return RunScriptCore(reinterpret_cast<const byte*>(script), wcslen(script) * sizeof(wchar_t), LoadScriptFlag_None, sourceContext, sourceUrl, parseOnly, parseAttributes, isSourceModule, result);
}

CHAKRA_API JsParseScript(_In_z_ const wchar_t * script, _In_ JsSourceContext sourceContext, _In_z_ const wchar_t *sourceUrl, _Out_ JsValueRef * result)
{
    return RunScriptCore(script, sourceContext, sourceUrl, true, JsParseScriptAttributeNone, false, result);
}

CHAKRA_API JsParseScriptWithAttributes(
    _In_z_ const wchar_t *script,
    _In_ JsSourceContext sourceContext,
    _In_z_ const wchar_t *sourceUrl,
    _In_ JsParseScriptAttributes parseAttributes,
    _Out_ JsValueRef *result)
{
    return RunScriptCore(script, sourceContext, sourceUrl, true, parseAttributes, false, result);
}

CHAKRA_API JsRunScript(_In_z_ const wchar_t * script, _In_ JsSourceContext sourceContext, _In_z_ const wchar_t *sourceUrl, _Out_ JsValueRef * result)
{
    return RunScriptCore(script, sourceContext, sourceUrl, false, JsParseScriptAttributeNone, false, result);
}

CHAKRA_API JsExperimentalApiRunModule(_In_z_ const wchar_t * script, _In_ JsSourceContext sourceContext, _In_z_ const wchar_t *sourceUrl, _Out_ JsValueRef * result)
{
    return RunScriptCore(script, sourceContext, sourceUrl, false, JsParseScriptAttributeNone, true, result);
}

JsErrorCode JsSerializeScriptCore(const byte *script, size_t cb, LoadScriptFlag loadScriptFlag, BYTE *functionTable, int functionTableSize, unsigned char *buffer, unsigned int *bufferSize)
{
    Js::JavascriptFunction *function;
    CompileScriptException se;

    JsErrorCode errorCode = ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        PARAM_NOT_NULL(script);
        PARAM_NOT_NULL(bufferSize);

        if (*bufferSize > 0)
        {
            PARAM_NOT_NULL(buffer);
            ZeroMemory(buffer, *bufferSize);
        }

        if (scriptContext->IsScriptContextInDebugMode())
        {
            return JsErrorCannotSerializeDebugScript;
        }

        SourceContextInfo * sourceContextInfo = scriptContext->GetSourceContextInfo(JS_SOURCE_CONTEXT_NONE, nullptr);
        Assert(sourceContextInfo != nullptr);

        const int chsize = (loadScriptFlag & LoadScriptFlag_Utf8Source) ? sizeof(utf8char_t) : sizeof(wchar_t);
        SRCINFO si = {
            /* sourceContextInfo   */ sourceContextInfo,
            /* dlnHost             */ 0,
            /* ulColumnHost        */ 0,
            /* lnMinHost           */ 0,
            /* ichMinHost          */ 0,
            /* ichLimHost          */ static_cast<ULONG>(cb / chsize), // OK to truncate since this is used to limit sourceText in debugDocument/compilation errors.
            /* ulCharOffset        */ 0,
            /* mod                 */ kmodGlobal,
            /* grfsi               */ 0
        };
        bool isSerializeByteCodeForLibrary = false;
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
        isSerializeByteCodeForLibrary = JsrtContext::GetCurrent()->GetRuntime()->IsSerializeByteCodeForLibrary();
#endif

        Js::Utf8SourceInfo* sourceInfo = nullptr;
        loadScriptFlag = (LoadScriptFlag)(loadScriptFlag | LoadScriptFlag_disableDeferredParse);
        if (isSerializeByteCodeForLibrary)
        {
            loadScriptFlag = (LoadScriptFlag)(loadScriptFlag | LoadScriptFlag_isByteCodeBufferForLibrary);
        }
        else
        {
            loadScriptFlag = (LoadScriptFlag)(loadScriptFlag | LoadScriptFlag_Expression);
        }
        function = scriptContext->LoadScript(script, cb, &si, &se, &sourceInfo, Js::Constants::GlobalCode, loadScriptFlag);
        return JsNoError;
    });

    if (errorCode != JsNoError)
    {
        return errorCode;
    }

    return ContextAPIWrapper<false>([&](Js::ScriptContext* scriptContext) -> JsErrorCode {
        if (function == nullptr)
        {
            HandleScriptCompileError(scriptContext, &se);
            return JsErrorScriptCompile;
        }
        // Could we have a deserialized function in this case?
        // If we are going to serialize it, a check isn't to expensive
        if (CONFIG_FLAG(ForceSerialized) && function->GetFunctionProxy() != nullptr) {
            function->GetFunctionProxy()->EnsureDeserialized();
        }
        Js::FunctionBody *functionBody = function->GetFunctionBody();
        const Js::Utf8SourceInfo *sourceInfo = functionBody->GetUtf8SourceInfo();
        size_t cSourceCodeLength = sourceInfo->GetCbLength(_u("JsSerializeScript"));

        // truncation of code length can lead to accessing random memory. Reject the call.
        if (cSourceCodeLength > DWORD_MAX)
        {
            return JsErrorOutOfMemory;
        }

        LPCUTF8 utf8Code = sourceInfo->GetSource(_u("JsSerializeScript"));
        DWORD dwFlags = 0;
#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
        dwFlags = JsrtContext::GetCurrent()->GetRuntime()->IsSerializeByteCodeForLibrary() ? GENERATE_BYTE_CODE_BUFFER_LIBRARY : 0;
#endif

        BEGIN_TEMP_ALLOCATOR(tempAllocator, scriptContext, _u("ByteCodeSerializer"));
        // We cast buffer size to DWORD* because on Windows, DWORD = unsigned long = unsigned int
        // On 64-bit clang on linux, this is not true, unsigned long is larger than unsigned int
        // However, the PAL defines DWORD for us on linux as unsigned int so the cast is safe here.
        HRESULT hr = Js::ByteCodeSerializer::SerializeToBuffer(scriptContext, tempAllocator, static_cast<DWORD>(cSourceCodeLength), utf8Code, functionBody, functionBody->GetHostSrcInfo(), false, &buffer, (DWORD*) bufferSize, dwFlags);
        END_TEMP_ALLOCATOR(tempAllocator, scriptContext);

        if (SUCCEEDED(hr))
        {
            return JsNoError;
        }
        else
        {
            return JsErrorScriptCompile;
        }
    });

}

CHAKRA_API JsSerializeScript(_In_z_ const wchar_t *script, _Out_writes_to_opt_(*bufferSize, *bufferSize) unsigned char *buffer,
    _Inout_ unsigned int *bufferSize)
{
    return JsSerializeScriptCore((const byte*)script, wcslen(script) * sizeof(wchar_t), LoadScriptFlag_None, nullptr, 0, buffer, bufferSize);
}

template <typename TLoadCallback, typename TUnloadCallback>
JsErrorCode RunSerializedScriptCore(
    TLoadCallback scriptLoadCallback, TUnloadCallback scriptUnloadCallback,
    JsSourceContext scriptLoadSourceContext, // only used by scriptLoadCallback
    unsigned char *buffer,
    JsSourceContext sourceContext, const wchar_t *sourceUrl,
    bool parseOnly, JsValueRef *result)
{
    Js::JavascriptFunction *function;
    JsErrorCode errorCode = ContextAPINoScriptWrapper([&](Js::ScriptContext *scriptContext) -> JsErrorCode {
        if (result != nullptr)
        {
            *result = nullptr;
        }

        PARAM_NOT_NULL(buffer);
        PARAM_NOT_NULL(sourceUrl);

        Js::ISourceHolder *sourceHolder = nullptr;
        PARAM_NOT_NULL(scriptLoadCallback);
        PARAM_NOT_NULL(scriptUnloadCallback);
        typedef Js::JsrtSourceHolder<TLoadCallback, TUnloadCallback> TSourceHolder;
        sourceHolder = RecyclerNewFinalized(scriptContext->GetRecycler(), TSourceHolder, scriptLoadCallback, scriptUnloadCallback, scriptLoadSourceContext);

        SourceContextInfo *sourceContextInfo;
        SRCINFO *hsi;
        Js::FunctionBody *functionBody = nullptr;

        HRESULT hr;

        sourceContextInfo = scriptContext->GetSourceContextInfo(sourceContext, nullptr);

        if (sourceContextInfo == nullptr)
        {
            sourceContextInfo = scriptContext->CreateSourceContextInfo(sourceContext, sourceUrl, wcslen(sourceUrl), nullptr);
        }

        SRCINFO si = {
            /* sourceContextInfo   */ sourceContextInfo,
            /* dlnHost             */ 0,
            /* ulColumnHost        */ 0,
            /* lnMinHost           */ 0,
            /* ichMinHost          */ 0,
            /* ichLimHost          */ 0, // xplat-todo: need to compute this?
            /* ulCharOffset        */ 0,
            /* mod                 */ kmodGlobal,
            /* grfsi               */ 0
        };

        uint32 flags = 0;

        if (CONFIG_FLAG(CreateFunctionProxy) && !scriptContext->IsProfiling())
        {
            flags = fscrAllowFunctionProxy;
        }

        hsi = scriptContext->AddHostSrcInfo(&si);
        hr = Js::ByteCodeSerializer::DeserializeFromBuffer(scriptContext, flags, sourceHolder, hsi, buffer, nullptr, &functionBody);

        if (FAILED(hr))
        {
            return JsErrorBadSerializedScript;
        }

        function = scriptContext->GetLibrary()->CreateScriptFunction(functionBody);

        JsrtContext * context = JsrtContext::GetCurrent();
        context->OnScriptLoad(function, functionBody->GetUtf8SourceInfo(), nullptr);

        return JsNoError;
    });

    if (errorCode != JsNoError)
    {
        return errorCode;
    }

    return ContextAPIWrapper<false>([&](Js::ScriptContext* scriptContext) -> JsErrorCode {
        if (parseOnly)
        {
            PARAM_NOT_NULL(result);
            *result = function;
        }
        else
        {
            Js::Var varResult = function->CallRootFunction(Js::Arguments(0, nullptr), scriptContext, true);
            if (result != nullptr)
            {
                *result = varResult;
            }
        }
        return JsNoError;
    });
}

#ifdef _WIN32

static bool CHAKRA_CALLBACK DummyScriptLoadSourceCallback(_In_ JsSourceContext sourceContext, _Outptr_result_z_ const wchar_t** scriptBuffer)
{
    // sourceContext is actually the script source pointer
    *scriptBuffer = reinterpret_cast<const wchar_t*>(sourceContext);
    return true;
}

static void CHAKRA_CALLBACK DummyScriptUnloadCallback(_In_ JsSourceContext sourceContext)
{
    // Do nothing
}

CHAKRA_API JsParseSerializedScript(_In_z_ const wchar_t * script, _In_ unsigned char *buffer, _In_ JsSourceContext sourceContext,
    _In_z_ const wchar_t *sourceUrl, _Out_ JsValueRef * result)
{
    return RunSerializedScriptCore(
        DummyScriptLoadSourceCallback, DummyScriptUnloadCallback,
        reinterpret_cast<JsSourceContext>(script), // use script source pointer as scriptLoadSourceContext
        buffer, sourceContext, sourceUrl, true, result);
}

CHAKRA_API JsRunSerializedScript(_In_z_ const wchar_t * script, _In_ unsigned char *buffer, _In_ JsSourceContext sourceContext,
    _In_z_ const wchar_t *sourceUrl, _Out_ JsValueRef * result)
{
    return RunSerializedScriptCore(
        DummyScriptLoadSourceCallback, DummyScriptUnloadCallback,
        reinterpret_cast<JsSourceContext>(script), // use script source pointer as scriptLoadSourceContext
        buffer, sourceContext, sourceUrl, false, result);
}

CHAKRA_API JsParseSerializedScriptWithCallback(_In_ JsSerializedScriptLoadSourceCallback scriptLoadCallback, _In_ JsSerializedScriptUnloadCallback scriptUnloadCallback, _In_ unsigned char *buffer, _In_ JsSourceContext sourceContext, _In_z_ const wchar_t *sourceUrl, _Out_ JsValueRef * result)
{
    return RunSerializedScriptCore(
        scriptLoadCallback, scriptUnloadCallback,
        sourceContext, // use the same user provided sourceContext as scriptLoadSourceContext
        buffer, sourceContext, sourceUrl, true, result);
}

CHAKRA_API JsRunSerializedScriptWithCallback(_In_ JsSerializedScriptLoadSourceCallback scriptLoadCallback, _In_ JsSerializedScriptUnloadCallback scriptUnloadCallback, _In_ unsigned char *buffer, _In_ JsSourceContext sourceContext, _In_z_ const wchar_t *sourceUrl, _Out_opt_ JsValueRef * result)
{
    return RunSerializedScriptCore(
        scriptLoadCallback, scriptUnloadCallback,
        sourceContext, // use the same user provided sourceContext as scriptLoadSourceContext
        buffer, sourceContext, sourceUrl, false, result);
}
#endif // _WIN32

CHAKRA_API JsParseScriptUtf8(
    _In_z_ const char *script,
    _In_ JsSourceContext sourceContext,
    _In_z_ const char *sourceUrl,
    _Out_ JsValueRef *result)
{
    return RunScriptCore(script, sourceContext, sourceUrl, /*parseOnly*/true,
            JsParseScriptAttributeNone, /*isSourceModule*/false, result);
}

CHAKRA_API JsRunScriptUtf8(
    _In_z_ const char *script,
    _In_ JsSourceContext sourceContext,
    _In_z_ const char *sourceUrl,
    _Out_ JsValueRef *result)
{
    return RunScriptCore(script, sourceContext, sourceUrl, false, JsParseScriptAttributeNone, false, result);
}

CHAKRA_API JsSerializeScriptUtf8(
    _In_z_ const char *script,
    _Out_writes_to_opt_(*bufferSize, *bufferSize) ChakraBytePtr buffer,
    _Inout_ unsigned int *bufferSize)
{
    return JsSerializeScriptCore((const byte*)script, strlen(script), LoadScriptFlag_Utf8Source, nullptr, 0, buffer, bufferSize);
}

CHAKRA_API JsRunSerializedScriptUtf8(
    _In_ JsSerializedScriptLoadUtf8SourceCallback scriptLoadCallback,
    _In_ JsSerializedScriptUnloadCallback scriptUnloadCallback,
    _In_ ChakraBytePtr buffer,
    _In_ JsSourceContext sourceContext,
    _In_z_ const char *sourceUrl,
    _Out_opt_ JsValueRef * result)
{
    utf8::NarrowToWide url(sourceUrl);
    if (!url)
    {
        return JsErrorOutOfMemory;
    }

    return RunSerializedScriptCore(
        scriptLoadCallback, scriptUnloadCallback,
        sourceContext, // use the same user provided sourceContext as scriptLoadSourceContext
        buffer, sourceContext, url, false, result);
}

CHAKRA_API JsExperimentalApiRunModuleUtf8(
    _In_z_ const char *script,
    _In_ JsSourceContext sourceContext,
    _In_z_ const char *sourceUrl,
    _Out_ JsValueRef *result)
{
    return RunScriptCore(script, sourceContext, sourceUrl, false, JsParseScriptAttributeNone, true, result);
}
