// Copyright Microsoft. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and / or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include <stdarg.h>
#include "jsrtutils.h"

namespace jsrt {

JsErrorCode UintToValue(uint32_t value, JsValueRef* result) {
  if (static_cast<int>(value) >= 0) {
    return JsIntToNumber(static_cast<int>(value), result);
  }

  // Otherwise doesn't fit int, use double
  return JsDoubleToNumber(value, result);
}

JsErrorCode GetProperty(JsValueRef ref,
                        JsValueRef propName,
                        JsValueRef *result) {
  JsPropertyIdRef idRef;
  JsErrorCode error;

  error = GetPropertyIdFromName(propName, &idRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsGetProperty(ref, idRef, result);

  return error;
}

JsErrorCode GetProperty(JsValueRef ref,
                        const char *propertyName,
                        JsValueRef *result) {
  JsPropertyIdRef idRef;
  JsErrorCode error;

  error = JsGetPropertyIdFromNameUtf8(propertyName, &idRef);

  if (error != JsNoError) {
    return error;
  }

  error = JsGetProperty(ref, idRef, result);

  return error;
}

JsErrorCode GetProperty(JsValueRef ref,
                        CachedPropertyIdRef cachedIdRef,
                        JsValueRef *result) {
  JsPropertyIdRef idRef =
    IsolateShim::GetCurrent()->GetCachedPropertyIdRef(cachedIdRef);

  return JsGetProperty(ref, idRef, result);
}

JsErrorCode GetProperty(JsValueRef ref,
                        JsPropertyIdRef propId,
                        int *intValue) {
  JsValueRef value;
  JsErrorCode error = JsGetProperty(ref, propId, &value);
  if (error != JsNoError) {
    return error;
  }

  return ValueToIntLikely(value, intValue);
}

JsErrorCode SetProperty(JsValueRef ref,
                        CachedPropertyIdRef cachedIdRef,
                        JsValueRef propValue) {
  JsPropertyIdRef idRef =
    IsolateShim::GetCurrent()->GetCachedPropertyIdRef(cachedIdRef);
  return JsSetProperty(ref, idRef, propValue, false);
}

JsErrorCode SetProperty(JsValueRef ref,
                        JsValueRef propName,
                        JsValueRef propValue) {
  JsPropertyIdRef idRef;
  JsErrorCode error;

  error = GetPropertyIdFromName(propName, &idRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsSetProperty(ref, idRef, propValue, false);

  return error;
}

JsErrorCode DeleteProperty(JsValueRef ref,
                           JsValueRef propName,
                           JsValueRef* result) {
  JsPropertyIdRef idRef;
  JsErrorCode error;

  error = GetPropertyIdFromName(propName, &idRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsDeleteProperty(ref, idRef, false, result);

  return error;
}

JsErrorCode CallProperty(JsValueRef ref,
                         CachedPropertyIdRef cachedIdRef,
                         JsValueRef *arguments,
                         unsigned short argumentCount,
                         JsValueRef *result) {
  JsValueRef propertyRef;
  JsErrorCode error;

  error = JsGetProperty(ref,
    jsrt::IsolateShim::GetCurrent()->GetCachedPropertyIdRef(cachedIdRef),
    &propertyRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsCallFunction(propertyRef, arguments, argumentCount, result);
  return error;
}

JsErrorCode CallGetter(JsValueRef ref,
                       CachedPropertyIdRef cachedIdRef,
                       JsValueRef* result) {
  JsValueRef args[] = { ref };
  return CallProperty(ref, cachedIdRef, args, _countof(args), result);
}

JsErrorCode CallGetter(JsValueRef ref,
                       CachedPropertyIdRef cachedIdRef,
                       int* result) {
  JsValueRef value;
  JsErrorCode error = CallGetter(ref, cachedIdRef, &value);
  if (error != JsNoError) {
    return error;
  }

  return ValueToIntLikely(value, result);
}

JsErrorCode GetPropertyOfGlobal(const char *propertyName,
                                JsValueRef *ref) {
  JsErrorCode error;
  JsPropertyIdRef propertyIdRef, globalRef;

  error = JsGetPropertyIdFromNameUtf8(propertyName, &propertyIdRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsGetGlobalObject(&globalRef);

  if (error != JsNoError) {
    return error;
  }

  return JsGetProperty(globalRef, propertyIdRef, ref);
}

JsErrorCode SetPropertyOfGlobal(const char *propertyName,
                                JsValueRef ref) {
  JsErrorCode error;
  JsPropertyIdRef propertyIdRef, globalRef;

  error = JsGetPropertyIdFromNameUtf8(propertyName, &propertyIdRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsGetGlobalObject(&globalRef);

  if (error != JsNoError) {
    return error;
  }

  return JsSetProperty(globalRef, propertyIdRef, ref, false);
}

JsValueRef GetTrue() {
  return ContextShim::GetCurrent()->GetTrue();
}

JsValueRef GetFalse() {
  return ContextShim::GetCurrent()->GetFalse();
}

JsValueRef GetUndefined() {
  return ContextShim::GetCurrent()->GetUndefined();
}

JsValueRef GetNull() {
  return ContextShim::GetCurrent()->GetNull();
}

JsErrorCode GetArrayLength(JsValueRef arrayRef,
                           unsigned int *arraySize) {
  JsErrorCode error;

  JsPropertyIdRef arrayLengthPropertyIdRef =
    IsolateShim::GetCurrent()->GetCachedPropertyIdRef(
      CachedPropertyIdRef::length);
  JsValueRef lengthRef;

  error = JsGetProperty(arrayRef, arrayLengthPropertyIdRef, &lengthRef);
  if (error != JsNoError) {
    return error;
  }

  double sizeInDouble;
  error = JsNumberToDouble(lengthRef, &sizeInDouble);
  *arraySize = static_cast<unsigned int>(sizeInDouble);

  return error;
}

bool InstanceOf(JsValueRef first,
                JsValueRef second) {
  bool result;
  return JsInstanceOf(first, second, &result) == JsNoError && result;
}

JsErrorCode CloneObject(JsValueRef source,
                        JsValueRef target,
                        bool clonePrototype) {
  JsValueRef cloneObjectFunction =
    ContextShim::GetCurrent()->GetcloneObjectFunction();

  JsValueRef resultRef;
  JsErrorCode error = CallFunction(cloneObjectFunction,
                                   source, target, &resultRef);
  if (error != JsNoError) {
    return error;
  }

  if (clonePrototype) {
    JsValueRef prototypeRef;
    JsErrorCode error = JsGetPrototype(source, &prototypeRef);
    if (error != JsNoError) {
      return error;
    }
    error = JsSetPrototype(target, prototypeRef);
  }

  return error;
}

JsErrorCode HasOwnProperty(JsValueRef object,
                           JsValueRef prop,
                           JsValueRef *result) {
  JsValueRef hasOwnPropertyFunction =
    ContextShim::GetCurrent()->GetGlobalPrototypeFunction(
      ContextShim::GlobalPrototypeFunction::Object_hasOwnProperty);

  JsValueRef args[] = { object, prop };
  return JsCallFunction(hasOwnPropertyFunction, args, _countof(args), result);
}

JsErrorCode IsValueInArray(
    JsValueRef arrayRef,
    JsValueRef valueRef,
    std::function<JsErrorCode(JsValueRef, JsValueRef, bool*)> comparator,
    bool* result) {
  JsErrorCode error;
  unsigned int length;
  *result = false;
  error = GetArrayLength(arrayRef, &length);
  if (error != JsNoError) {
    return error;
  }

  for (unsigned int index = 0; index < length; index++) {
    JsValueRef indexValue;
    error = JsIntToNumber(index, &indexValue);
    if (error != JsNoError) {
      return error;
    }

    JsValueRef itemRef;
    error = JsGetIndexedProperty(arrayRef, indexValue, &itemRef);
    if (error != JsNoError) {
      return error;
    }

    if (comparator != nullptr) {
      error = comparator(valueRef, itemRef, result);
    } else {
      error = JsEquals(itemRef, valueRef, result);
    }

    if (error != JsNoError) {
      return error;
    }

    if (*result)
      return JsNoError;
  }

  return JsNoError;
}

JsErrorCode IsValueInArray(JsValueRef arrayRef,
                           JsValueRef valueRef,
                           bool* result) {
  return IsValueInArray(arrayRef, valueRef, nullptr, result);
}

JsErrorCode IsCaseInsensitiveStringValueInArray(JsValueRef arrayRef,
                                                JsValueRef valueRef,
                                                bool* result) {
  return IsValueInArray(arrayRef, valueRef, [=](
      JsValueRef first, JsValueRef second, bool* areEqual) -> JsErrorCode {
    JsValueType type;
    *areEqual = false;

    JsErrorCode error = JsGetValueType(first, &type);
    if (error != JsNoError) {
      return error;
    }
    if (type != JsString) {
      return JsNoError;
    }

    error = JsGetValueType(second, &type);
    if (error != JsNoError) {
      return error;
    }
    if (type != JsString) {
      return JsNoError;
    }

    const char* firstPtr;
    size_t firstLength;

    error = JsStringToPointerUtf8(first, &firstPtr, &firstLength);
    if (error != JsNoError) {
      return error;
    }

    const char* secondPtr;
    size_t secondLength;

    error = JsStringToPointerUtf8(second, &secondPtr, &secondLength);
    if (error != JsNoError) {
      return error;
    }

    size_t maxCount = min(firstLength, secondLength);
    *areEqual = (strnicmp(firstPtr, secondPtr, maxCount) == 0);
    return JsNoError;
  },
                        result);
}

JsErrorCode GetOwnPropertyDescriptor(JsValueRef ref,
                                     JsValueRef prop,
                                     JsValueRef* result) {
  return CallFunction(
    ContextShim::GetCurrent()->GetGetOwnPropertyDescriptorFunction(),
    ref, prop, result);
}

JsErrorCode IsZero(JsValueRef value,
                   bool *result) {
  return JsEquals(value, ContextShim::GetCurrent()->GetZero(), result);
}

JsErrorCode IsUndefined(JsValueRef value,
                        bool *result) {
  return JsEquals(value, GetUndefined(), result);
}

JsErrorCode GetEnumerableNamedProperties(JsValueRef object,
                                         JsValueRef *result) {
  return CallFunction(
    ContextShim::GetCurrent()->GetgetEnumerableNamedPropertiesFunction(),
    object, result);
}

JsErrorCode GetEnumerableIndexedProperties(JsValueRef object,
                                           JsValueRef *result) {
  return CallFunction(
    ContextShim::GetCurrent()->GetgetEnumerableIndexedPropertiesFunction(),
    object, result);
}

JsErrorCode GetIndexedOwnKeys(JsValueRef object,
                              JsValueRef *result) {
  return CallFunction(
    ContextShim::GetCurrent()->GetgetIndexedOwnKeysFunction(),
    object, result);
}

JsErrorCode GetNamedOwnKeys(JsValueRef object,
                            JsValueRef *result) {
  return CallFunction(
    ContextShim::GetCurrent()->GetgetNamedOwnKeysFunction(),
    object, result);
}

JsErrorCode ConcatArray(JsValueRef first,
                        JsValueRef second,
                        JsValueRef *result) {
  JsValueRef args[] = { first, second };

  return CallProperty(first,
                      CachedPropertyIdRef::concat,
                      args, _countof(args), result);
}

JsErrorCode CreateEnumerationIterator(JsValueRef enumeration,
                                      JsValueRef *result) {
  return CallFunction(
    ContextShim::GetCurrent()->GetcreateEnumerationIteratorFunction(),
    enumeration, result);
}

JsErrorCode CreatePropertyDescriptorsEnumerationIterator(JsValueRef enumeration,
                                                         JsValueRef *result) {
  return CallFunction(
    ContextShim::GetCurrent()
      ->GetcreatePropertyDescriptorsEnumerationIteratorFunction(),
    enumeration, result);
}

JsErrorCode GetPropertyNames(JsValueRef object,
                             JsValueRef *result) {
  return CallFunction(
    ContextShim::GetCurrent()->GetgetPropertyNamesFunction(),
    object, result);
}

JsErrorCode AddExternalData(JsValueRef ref,
                            JsPropertyIdRef externalDataPropertyId,
                            void *data,
                            JsFinalizeCallback onObjectFinalize) {
  JsErrorCode error;

  JsValueRef externalObjectRef;
  error = JsCreateExternalObject(data, onObjectFinalize, &externalObjectRef);
  if (error != JsNoError) {
    return error;
  }

  error = DefineProperty(ref,
                         externalDataPropertyId,
                         PropertyDescriptorOptionValues::False,
                         PropertyDescriptorOptionValues::False,
                         PropertyDescriptorOptionValues::False,
                         externalObjectRef,
                         JS_INVALID_REFERENCE, JS_INVALID_REFERENCE);
  return error;
}

JsErrorCode AddExternalData(JsValueRef ref,
                            void *data,
                            JsFinalizeCallback onObjectFinalize) {
  IsolateShim* iso = IsolateShim::GetCurrent();
  JsPropertyIdRef propId = iso->GetCachedSymbolPropertyIdRef(
    CachedSymbolPropertyIdRef::__external__);

  return AddExternalData(ref, propId, data, onObjectFinalize);
}

JsErrorCode GetExternalData(JsValueRef ref,
                            JsPropertyIdRef idRef,
                            void **data) {
  JsErrorCode error;

  JsValueRef externalObject;
  error = JsGetProperty(ref, idRef, &externalObject);
  if (error != JsNoError) {
    return error;
  }

  error = JsGetExternalData(externalObject, data);
  if (error == JsErrorInvalidArgument) {
    *data = nullptr;
    error = JsNoError;
  }

  return error;
}

JsErrorCode GetExternalData(JsValueRef ref,
                            void **data) {
  IsolateShim* iso = IsolateShim::GetCurrent();
  JsPropertyIdRef propId = iso->GetCachedSymbolPropertyIdRef(
    CachedSymbolPropertyIdRef::__external__);

  return GetExternalData(ref, propId, data);
}

JsErrorCode CreateFunctionWithExternalData(
    JsNativeFunction nativeFunction,
    void* data,
    JsFinalizeCallback onObjectFinalize,
    JsValueRef *function) {
  JsErrorCode error;
  error = JsCreateFunction(nativeFunction, nullptr, function);
  if (error != JsNoError) {
    return error;
  }

  error = AddExternalData(*function, data, onObjectFinalize);
  return error;
}

JsErrorCode ToString(JsValueRef ref,
                     JsValueRef * strRef,
                     const char** str,
                     bool alreadyString) {
  // just a dummy here
  size_t size;
  JsErrorCode error;

  // call convert only if needed
  if (alreadyString) {
    *strRef = ref;
  } else {
    error = JsConvertValueToString(ref, strRef);
    if (error != JsNoError) {
      return error;
    }
  }

  error = JsStringToPointerUtf8(*strRef, str, &size);
  return error;
}


#define DEF_IS_TYPE(F) \
JsErrorCode Call##F(JsValueRef value, JsValueRef *resultRef) { \
  return CallFunction( \
    ContextShim::GetCurrent()->Get##F##Function(), \
    value, resultRef); \
} \

#include "jsrtcachedpropertyidref.inc"
#undef DEF_IS_TYPE

PropertyDescriptorOptionValues GetPropertyDescriptorOptionValue(bool b) {
  return b ?
    PropertyDescriptorOptionValues::True :
    PropertyDescriptorOptionValues::False;
}

JsErrorCode CreatePropertyDescriptor(
    PropertyDescriptorOptionValues writable,
    PropertyDescriptorOptionValues enumerable,
    PropertyDescriptorOptionValues configurable,
    JsValueRef value,
    JsValueRef getter,
    JsValueRef setter,
    JsValueRef *descriptor) {
  JsErrorCode error;
  error = JsCreateObject(descriptor);
  if (error != JsNoError) {
    return error;
  }

  IsolateShim * isolateShim = IsolateShim::GetCurrent();
  ContextShim * contextShim = isolateShim->GetCurrentContextShim();
  JsValueRef trueRef = contextShim->GetTrue();
  JsValueRef falseRef = contextShim->GetFalse();

  // set writable
  if (writable != PropertyDescriptorOptionValues::None) {
    JsPropertyIdRef writablePropertyIdRef =
      isolateShim->GetCachedPropertyIdRef(CachedPropertyIdRef::writable);
    JsValueRef writableRef =
      (writable == PropertyDescriptorOptionValues::True) ? trueRef : falseRef;
    error = JsSetProperty(*descriptor,
                          writablePropertyIdRef, writableRef, false);
    if (error != JsNoError) {
      return error;
    }
  }

  // set enumerable
  if (enumerable != PropertyDescriptorOptionValues::None) {
    JsPropertyIdRef enumerablePropertyIdRef =
      isolateShim->GetCachedPropertyIdRef(CachedPropertyIdRef::enumerable);
    JsValueRef enumerableRef =
      (enumerable == PropertyDescriptorOptionValues::True) ? trueRef : falseRef;
    error = JsSetProperty(*descriptor,
                          enumerablePropertyIdRef, enumerableRef, false);
    if (error != JsNoError) {
      return error;
    }
  }

  // set configurable
  if (configurable != PropertyDescriptorOptionValues::None) {
    JsPropertyIdRef configurablePropertyIdRef =
      isolateShim->GetCachedPropertyIdRef(CachedPropertyIdRef::configurable);
    JsValueRef configurableRef =
      (configurable == PropertyDescriptorOptionValues::True) ?
        trueRef : falseRef;
    error = JsSetProperty(*descriptor,
                          configurablePropertyIdRef, configurableRef, false);
    if (error != JsNoError) {
      return error;
    }
  }

  // set value
  if (value != JS_INVALID_REFERENCE) {
    JsPropertyIdRef valuePropertyIdRef =
      isolateShim->GetCachedPropertyIdRef(CachedPropertyIdRef::value);
    error = JsSetProperty(*descriptor, valuePropertyIdRef, value, false);
    if (error != JsNoError) {
      return error;
    }
  }

  // set getter if needed
  if (getter != JS_INVALID_REFERENCE) {
    JsPropertyIdRef getterPropertyIdRef =
      isolateShim->GetCachedPropertyIdRef(CachedPropertyIdRef::get);
    error = JsSetProperty(*descriptor, getterPropertyIdRef, getter, false);
    if (error != JsNoError) {
      return error;
    }
  }

  // set setter if needed
  if (setter != JS_INVALID_REFERENCE) {
    JsPropertyIdRef setterPropertyIdRef =
      isolateShim->GetCachedPropertyIdRef(CachedPropertyIdRef::set);
    error = JsSetProperty(*descriptor, setterPropertyIdRef, setter, false);
    if (error != JsNoError) {
      return error;
    }
  }

  return JsNoError;
}

JsErrorCode CreatePropertyDescriptor(v8::PropertyAttribute attributes,
                                     JsValueRef value,
                                     JsValueRef getter,
                                     JsValueRef setter,
                                     JsValueRef *descriptor) {
  return CreatePropertyDescriptor(
    GetPropertyDescriptorOptionValue(!(attributes & v8::ReadOnly)),
    GetPropertyDescriptorOptionValue(!(attributes & v8::DontEnum)),
    GetPropertyDescriptorOptionValue(!(attributes & v8::DontDelete)),
    value,
    JS_INVALID_REFERENCE,
    JS_INVALID_REFERENCE,
    descriptor);
}

JsErrorCode DefineProperty(JsValueRef object,
                           JsPropertyIdRef propertyIdRef,
                           PropertyDescriptorOptionValues writable,
                           PropertyDescriptorOptionValues enumerable,
                           PropertyDescriptorOptionValues configurable,
                           JsValueRef value,
                           JsValueRef getter,
                           JsValueRef setter) {
  JsValueRef descriptor;
  JsErrorCode error;
  error = CreatePropertyDescriptor(
    writable, enumerable, configurable, value, getter, setter, &descriptor);
  if (error != JsNoError) {
    return error;
  }

  bool result;
  error = JsDefineProperty(object, propertyIdRef, descriptor, &result);

  if (error == JsNoError && !result) {
    return JsErrorInvalidArgument;
  }
  return error;
}

JsErrorCode DefineProperty(JsValueRef object,
                           const char * propertyName,
                           PropertyDescriptorOptionValues writable,
                           PropertyDescriptorOptionValues enumerable,
                           PropertyDescriptorOptionValues configurable,
                           JsValueRef value,
                           JsValueRef getter,
                           JsValueRef setter) {
  JsErrorCode error;
  JsPropertyIdRef propertyIdRef;

  error = JsGetPropertyIdFromNameUtf8(propertyName, &propertyIdRef);
  if (error != JsNoError) {
    return error;
  }

  error = DefineProperty(
    object, propertyIdRef, writable, enumerable, configurable, value,
    getter, setter);
  return error;
}

JsErrorCode GetPropertyIdFromName(JsValueRef nameRef,
                                  JsPropertyIdRef *idRef) {
  JsErrorCode error;
  const char *propertyName;
  size_t propertyNameSize;

  // Expect the name be either a String or a Symbol.
  error = JsStringToPointerUtf8(nameRef, &propertyName, &propertyNameSize);
  if (error != JsNoError) {
    if (error == JsErrorInvalidArgument) {
      error = JsGetPropertyIdFromSymbol(nameRef, idRef);
      if (error == JsErrorPropertyNotSymbol) {
        error = JsErrorInvalidArgument;  // Neither String nor Symbol
      }
    }
  } else {
    error = JsGetPropertyIdFromNameUtf8(propertyName, idRef);
  }

  return error;
}

JsErrorCode GetPropertyIdFromValue(JsValueRef valueRef,
                                   JsPropertyIdRef *idRef) {
  JsErrorCode error;

  error = GetPropertyIdFromName(valueRef, idRef);
  if (error == JsErrorInvalidArgument) {
    error = JsConvertValueToString(valueRef, &valueRef);
    if (error != JsNoError) {
      return error;
    }

    error = GetPropertyIdFromName(valueRef, idRef);
  }

  return error;
}

JsErrorCode GetObjectConstructor(JsValueRef objectRef,
                                 JsValueRef *constructorRef) {
  IsolateShim* iso = IsolateShim::GetCurrent();
  JsPropertyIdRef constructorPropertyIdRef = iso->GetCachedPropertyIdRef(
    CachedPropertyIdRef::constructor);

  return JsGetProperty(objectRef, constructorPropertyIdRef, constructorRef);
}

JsErrorCode SetIndexedProperty(JsValueRef object,
                               unsigned int index,
                               JsValueRef value) {
  JsErrorCode error;
  JsValueRef indexRef;
  error = UintToValue(index, &indexRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsSetIndexedProperty(object, indexRef, value);
  return error;
}

JsErrorCode GetIndexedProperty(JsValueRef object,
                               unsigned int index,
                               JsValueRef *value) {
  JsErrorCode error;
  JsValueRef indexRef;
  error = UintToValue(index, &indexRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsGetIndexedProperty(object, indexRef, value);
  return error;
}

JsErrorCode DeleteIndexedProperty(JsValueRef object,
                                  unsigned int index) {
  JsErrorCode error;
  JsValueRef indexRef;
  error = UintToValue(index, &indexRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsDeleteIndexedProperty(object, indexRef);
  return error;
}

JsErrorCode HasProperty(JsValueRef object,
                        JsValueRef propName,
                        bool *result) {
  JsPropertyIdRef idRef;
  JsErrorCode error;

  error = GetPropertyIdFromName(propName, &idRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsHasProperty(object, idRef, result);

  return error;
}

JsErrorCode HasIndexedProperty(JsValueRef object,
                               unsigned int index,
                               bool *result) {
  JsErrorCode error;
  JsValueRef indexRef;
  error = UintToValue(index, &indexRef);
  if (error != JsNoError) {
    return error;
  }

  error = JsHasIndexedProperty(object, indexRef, result);
  return error;
}

JsErrorCode ParseScript(const char *script,
                        JsSourceContext sourceContext,
                        const char *sourceUrl,
                        bool isStrictMode,
                        JsValueRef *result) {
  if (isStrictMode) {
    // do not append new line so the line numbers on error stack are correct
    std::string useStrictTag("'use strict'; ");
    return JsParseScriptUtf8(useStrictTag.append(script).c_str(), sourceContext,
                  sourceUrl, result);
  } else {
    return JsParseScriptUtf8(script, sourceContext, sourceUrl, result);
  }
}

#define RETURN_IF_JSERROR(err, returnValue) \
if (err != JsNoError) { \
  return returnValue; \
}

JsErrorCode GetHiddenValuesTable(JsValueRef object,
                                JsPropertyIdRef* hiddenValueIdRef,
                                JsValueRef* hiddenValuesTable,
                                bool* isUndefined) {
  *isUndefined = true;
  IsolateShim* iso = IsolateShim::GetCurrent();
  *hiddenValueIdRef = iso->GetCachedSymbolPropertyIdRef(
    CachedSymbolPropertyIdRef::__hiddenvalues__);
  JsErrorCode errorCode;

  errorCode = JsGetProperty(object, *hiddenValueIdRef, hiddenValuesTable);
  RETURN_IF_JSERROR(errorCode, errorCode);

  errorCode = IsUndefined(*hiddenValuesTable, isUndefined);
  RETURN_IF_JSERROR(errorCode, errorCode);

  return JsNoError;
}

bool HasPrivate(JsValueRef object, JsValueRef key) {
  JsPropertyIdRef hiddenValuesIdRef;
  JsValueRef hiddenValuesTable;
  JsErrorCode errorCode;
  bool isUndefined;

  errorCode = GetHiddenValuesTable(object, &hiddenValuesIdRef,
                                  &hiddenValuesTable, &isUndefined);
  RETURN_IF_JSERROR(errorCode, false);

  if (isUndefined) {
    return false;
  }

  JsValueRef hasPropertyRef;
  errorCode = jsrt::HasOwnProperty(hiddenValuesTable, key, &hasPropertyRef);
  RETURN_IF_JSERROR(errorCode, false);

  bool hasKey;
  errorCode = JsBooleanToBool(hasPropertyRef, &hasKey);
  RETURN_IF_JSERROR(errorCode, false);

  return hasKey;
}

bool DeletePrivate(JsValueRef object, JsValueRef key) {
  JsPropertyIdRef hiddenValuesIdRef;
  JsValueRef hiddenValuesTable;
  JsErrorCode errorCode;
  bool isUndefined;

  errorCode = GetHiddenValuesTable(object, &hiddenValuesIdRef,
                                  &hiddenValuesTable, &isUndefined);
  RETURN_IF_JSERROR(errorCode, false);

  if (isUndefined) {
    return false;
  }

  JsValueRef deleteResultRef;
  errorCode = jsrt::DeleteProperty(hiddenValuesTable, key, &deleteResultRef);
  RETURN_IF_JSERROR(errorCode, false);

  bool hasDeleted;
  errorCode = JsBooleanToBool(deleteResultRef, &hasDeleted);
  RETURN_IF_JSERROR(errorCode, false);

  return hasDeleted;
}

JsErrorCode GetPrivate(JsValueRef object, JsValueRef key,
                       JsValueRef *result) {
  JsPropertyIdRef hiddenValuesIdRef;
  JsValueRef hiddenValuesTable;
  JsErrorCode errorCode;
  JsValueRef undefinedValueRef = GetUndefined();
  bool isUndefined;

  errorCode = GetHiddenValuesTable(object, &hiddenValuesIdRef,
                                  &hiddenValuesTable, &isUndefined);
  RETURN_IF_JSERROR(errorCode, errorCode);

  if (isUndefined) {
      *result = undefinedValueRef;
      return JsNoError;
  }

  JsPropertyIdRef keyIdRef;
  errorCode = GetPropertyIdFromName(key, &keyIdRef);
  RETURN_IF_JSERROR(errorCode, errorCode);

  // Is 'key' present in hiddenValuesTable? If not, return undefined
  JsValueRef hasPropertyRef;
  errorCode = HasOwnProperty(hiddenValuesTable, key, &hasPropertyRef);
  RETURN_IF_JSERROR(errorCode, errorCode);

  bool hasKey;
  errorCode = JsBooleanToBool(hasPropertyRef, &hasKey);
  RETURN_IF_JSERROR(errorCode, errorCode);

  if (!hasKey) {
    *result = undefinedValueRef;
    return JsNoError;
  }

  errorCode = JsGetProperty(hiddenValuesTable, keyIdRef, result);
  RETURN_IF_JSERROR(errorCode, errorCode);

  return JsNoError;
}

JsErrorCode SetPrivate(JsValueRef object, JsValueRef key,
                           JsValueRef value) {
  JsPropertyIdRef hiddenValuesIdRef;
  JsValueRef hiddenValuesTable;
  JsErrorCode errorCode;
  bool isUndefined;

  errorCode = GetHiddenValuesTable(object, &hiddenValuesIdRef,
                                  &hiddenValuesTable, &isUndefined);
  RETURN_IF_JSERROR(errorCode, errorCode);

  // if '__hiddenvalues__' is not defined on object, define it
  if (isUndefined) {
    errorCode = JsCreateObject(&hiddenValuesTable);
    RETURN_IF_JSERROR(errorCode, errorCode);

    errorCode = DefineProperty(object, hiddenValuesIdRef,
                               PropertyDescriptorOptionValues::False,
                               PropertyDescriptorOptionValues::False,
                               PropertyDescriptorOptionValues::False,
                               hiddenValuesTable,
                               JS_INVALID_REFERENCE,
                               JS_INVALID_REFERENCE);
    RETURN_IF_JSERROR(errorCode, errorCode);
  }

  errorCode = jsrt::SetProperty(hiddenValuesTable, key, value);
  RETURN_IF_JSERROR(errorCode, errorCode);

  return JsNoError;
}

void Unimplemented(const char * message) {
  fprintf(stderr, "FATAL ERROR: '%s' unimplemented", message);
  __debugbreak();
  abort();
}

void Fatal(const char * format, ...) {
  bool hasException;
  JsErrorCode errorCode;
  JsValueRef exceptionRef;
  JsValueRef stackRef;
  JsValueRef strErrorRef;
  size_t stringLength;
  const char* strError;

  va_list args;
  va_start(args, format);
  fprintf(stderr, "FATAL ERROR: ");
  vfprintf(stderr, format, args);
  va_end(args);

  errorCode = JsHasException(&hasException);
  if (!hasException || errorCode != JsNoError) {
    if (errorCode != JsNoError)
      fprintf(stderr, "\nImportant: While trying to check Javascript "
        "exception, JsHasException has also failed.\n");
    else
      fprintf(stderr, "\nImportant: This didn't happen because of an "
        "uncaught Javascript exception.\n");
  } else if (JsGetAndClearException(&exceptionRef) == JsNoError &&
           GetProperty(exceptionRef, "stack", &stackRef) == JsNoError &&
           JsConvertValueToString(stackRef, &strErrorRef) == JsNoError  &&
           JsStringToPointerUtf8(strErrorRef, &strError,
                                 &stringLength) == JsNoError) {
    fprintf(stderr, "\n%s\n", strError);
  }

#ifdef DEBUG
  __debugbreak();
#endif

  abort();
}


JsValueRef CHAKRA_CALLBACK CollectGarbage(
  JsValueRef callee,
  bool isConstructCall,
  JsValueRef *arguments,
  unsigned short argumentCount,
  void *callbackState) {
  JsCollectGarbage(IsolateShim::GetCurrent()->GetRuntimeHandle());
  return jsrt::GetUndefined();
}

void IdleGC(uv_timer_t *timerHandler) {
#ifdef _WIN32
  unsigned int nextIdleTicks;
  CHAKRA_VERIFY(JsIdle(&nextIdleTicks) == JsNoError);
  DWORD currentTicks = GetTickCount();

  // If idleGc completed, we don't need to schedule anything.
  // simply reset the script execution flag so that idleGC
  // is retriggered only when scripts are executed.
  if (nextIdleTicks == UINT_MAX) {
    IsolateShim::GetCurrent()->ResetScriptExecuted();
    IsolateShim::GetCurrent()->ResetIsIdleGcScheduled();
    return;
  }

  // If IdleGC didn't complete, retry doing it after diff.
  if (nextIdleTicks > currentTicks) {
    unsigned int diff = nextIdleTicks - currentTicks;
    ScheduleIdleGcTask(diff);
  } else {
    IsolateShim::GetCurrent()->ResetIsIdleGcScheduled();
  }
#else
  // CHAKRA-TODO: implement. No GetTickCount()
  IsolateShim::GetCurrent()->ResetScriptExecuted();
  IsolateShim::GetCurrent()->ResetIsIdleGcScheduled();
#endif
}

void PrepareIdleGC(uv_prepare_t* prepareHandler) {
  // If there were no scripts executed, return
  if (!IsolateShim::GetCurrent()->IsJsScriptExecuted()) {
    return;
  }

  // If idleGC task already scheduled, return
  if (IsolateShim::GetCurrent()->IsIdleGcScheduled()) {
    return;
  }

  ScheduleIdleGcTask();
}

void ScheduleIdleGcTask(uint64_t timeoutInMilliSeconds) {
  uv_timer_start(IsolateShim::GetCurrent()->idleGc_timer_handle(),
                 IdleGC, timeoutInMilliSeconds, 0);
  IsolateShim::GetCurrent()->SetIsIdleGcScheduled();
}
}  // namespace jsrt

