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

#include "v8chakra.h"

#ifndef Assert
#define Assert CHAKRA_ASSERT
#endif
#include "Utf8Helper.h"

void __cdecl CodexAssert(bool condition) {
  Assert(condition);
}

void __cdecl CodexFailFast(bool condition) {
  Assert(condition);
  if (!condition) {
    jsrt::Fatal("ChakraCore codex failure");
  }
}

namespace v8 {

using std::unique_ptr;

String::Utf8Value::Utf8Value(Handle<v8::Value> obj)
    : _str(nullptr), _length(0) {
  Handle<String> str = obj->ToString();
  if (str.IsEmpty()) {
    return;
  }

  size_t len = 0;
  if (JsStringToPointerUtf8Copy(*str, &_str, &len) == JsNoError) {
    _length = static_cast<int>(len);
  }
}

String::Utf8Value::~Utf8Value() {
  if (_str != nullptr) {
    JsErrorCode err = JsStringFree(_str);
    CHAKRA_ASSERT(err == JsNoError);
    UNUSED(err);
  }
}

String::Value::Value(Handle<v8::Value> obj) : _str(nullptr), _length(0) {
  Handle<String> str = obj->ToString();
  if (str.IsEmpty()) {
    return;
  }

  _length = str->Length();
  _str = new uint16_t[_length + 1];
  str->Write(_str);
}

String::Value::~Value() {
  if (_str != nullptr) {
    delete _str;
  }
}

int String::Length() const {
  int length = 0;

  if (JsGetStringLength((JsValueRef)this, &length) != JsNoError) {
    return 0;
  }

  return length;
}

int String::Utf8Length() const {
  jsrt::StringUtf8 str;
  if (str.From((JsValueRef)this) != JsNoError) {
    // error
    return 0;
  }

  return str.length();
}

template <class CharType>
static int WriteRaw(
    JsValueRef ref, CharType* buffer, int start, int length, int options) {
  if (length == 0) {
    // bail out if we are required to write no chars
    return 0;
  }

  jsrt::StringUtf8 utf8Str;
  if (utf8Str.From(ref) != JsNoError) {
    // error
    return 0;
  }

  // Convert to wide string
  utf8::NarrowToWide str(utf8Str, utf8Str.length());

  int stringLength = str.Length();
  if (stringLength == 0) {
    if (!(options & String::NO_NULL_TERMINATION)) {
      // include the null terminate
      buffer[0] = L'\0';
    }
    return 0;
  }

  if (start < 0 || start > stringLength) {
    // illegal bail out
    return 0;
  }

  // in case length was not provided we want to copy the whole string
  int count = length >= 0 ? length : stringLength;
  count = min(count, stringLength - start);

  if (length < 0) {
    // If length was not provided, assume enough space to hold content and null
    // terminator.
    length = count + 1;
  }

  jsrt::StringConvert::CopyRaw(str + start, count, buffer, length);

  if (count < length && !(options & String::NO_NULL_TERMINATION)) {
    // include the null terminate
    buffer[count++] = L'\0';
  }

  return count;
}

int String::Write(uint16_t *buffer, int start, int length, int options) const {
  return WriteRaw(
    (JsValueRef)this, buffer, start, length, options);
}

int String::WriteOneByte(
    uint8_t* buffer, int start, int length, int options) const {
  return WriteRaw(
    (JsValueRef)this, buffer, start, length, options);
}

int String::WriteUtf8(
    char *buffer, int length, int *nchars_ref, int options) const {
  if (nchars_ref) {
    *nchars_ref = 0;
  }

  if (length == 0) {
    // bail out if we are required to write no chars
    return 0;
  }

  jsrt::StringUtf8 str;
  if (str.From((JsValueRef)this) != JsNoError) {
    // error
    return 0;
  }

  if (length < 0) {
    // in case length was not provided we want to copy the whole string
    length = str.length() + 1;
  }

  int count = str.length();
  if (count > length) {
    // in the horrible situation in which the buffer size isn't sufficient we
    // mimic the behavior of v8 and find the maximal number of characters that
    // we can fit in the given buffer
    auto maxFitChars = utf8::ByteIndexIntoCharacterIndex(
      (LPCUTF8)(const char*)str, length,
      utf8::DecodeOptions::doChunkedEncoding);
    count = utf8::CharacterIndexToByteIndex(
      (LPCUTF8)(const char*)str, str.length(), maxFitChars);
  }

  memmove(buffer, str, count);

  if (count < length && !(options & String::NO_NULL_TERMINATION)) {
    buffer[count++] = '\0';
  }

  if (nchars_ref != nullptr) {
    *nchars_ref = static_cast<int>(count);
  }

  return static_cast<int>(count);
}

Local<String> String::Empty(Isolate* isolate) {
  return FromMaybe(Utils::NewString("", 0));
}

String* String::Cast(v8::Value *obj) {
  CHAKRA_ASSERT(obj->IsString());
  return static_cast<String*>(obj);
}

MaybeLocal<String> Utils::NewString(const char *data, int length) {
  if (length < 0) {
    length = static_cast<int>(strlen(data));
  }

  JsValueRef strRef;
  if (JsPointerToStringUtf8(data, length, &strRef) != JsNoError) {
    return Local<String>();
  }

  return Local<String>::New(strRef);
}

MaybeLocal<String> String::NewFromUtf8(Isolate* isolate,
                                       const char* data,
                                       v8::NewStringType type,
                                       int length) {
  return Utils::NewString(data, length);
}

Local<String> String::NewFromUtf8(Isolate* isolate,
                                  const char* data,
                                  NewStringType type,
                                  int length) {
  return FromMaybe(NewFromUtf8(isolate, data,
                               static_cast<v8::NewStringType>(type), length));
}

MaybeLocal<String> String::NewFromOneByte(Isolate* isolate,
                                          const uint8_t* data,
                                          v8::NewStringType type,
                                          int length) {
  if (length < 0) {
    length = strlen((const char*)data);
  }

  unique_ptr<uint16_t[]> wdata(new uint16_t[length]);
  jsrt::StringConvert::CopyRaw(data, length, wdata.get(), length);

  return NewFromTwoByte(isolate, wdata.get(), type, length);
}

Local<String> String::NewFromOneByte(Isolate* isolate,
                                     const uint8_t* data,
                                     NewStringType type,
                                     int length) {
  return FromMaybe(NewFromOneByte(isolate, data,
                                  static_cast<v8::NewStringType>(type),
                                  length));
}

MaybeLocal<String> String::NewFromTwoByte(Isolate* isolate,
                                          const uint16_t* data,
                                          v8::NewStringType type,
                                          int length) {
  utf8::WideToNarrow str(reinterpret_cast<LPCWSTR>(data), length);
  return NewFromUtf8(isolate, str, type, str.Length());
}

Local<String> String::NewFromTwoByte(Isolate* isolate,
                                     const uint16_t* data,
                                     NewStringType type,
                                     int length) {
  return FromMaybe(NewFromTwoByte(isolate, data,
                                  static_cast<v8::NewStringType>(type),
                                  length));
}

Local<String> String::Concat(Handle<String> left, Handle<String> right) {
  JsValueRef args[] = { *left, *right };
  JsValueRef result;

  JsValueRef stringConcatFunction =
    jsrt::ContextShim::GetCurrent()->GetStringConcatFunction();
  JsErrorCode error = JsCallFunction(stringConcatFunction,
                                     args, _countof(args), &result);
  if (error != JsNoError) {
    return Local<String>();
  }

  return Local<String>::New(result);
}

MaybeLocal<String> String::NewExternalTwoByte(
    Isolate* isolate, ExternalStringResource* resource) {
  if (resource->data() != nullptr) {
    auto newStr = NewFromTwoByte(nullptr, resource->data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(resource->length()));
    delete resource;
    return newStr;
  }

  // otherwise the resource is empty just delete it and return an empty string
  delete resource;
  return Empty(nullptr);
}

Local<String> String::NewExternal(Isolate* isolate,
                                  ExternalStringResource* resource) {
  return FromMaybe(NewExternalTwoByte(isolate, resource));
}

MaybeLocal<String> String::NewExternalOneByte(
    Isolate* isolate, ExternalOneByteStringResource* resource) {
  if (resource->data() != nullptr) {
    auto newStr = NewFromOneByte(
      nullptr,
      reinterpret_cast<const uint8_t*>(resource->data()),
      v8::NewStringType::kNormal,
      static_cast<int>(resource->length()));

    delete resource;
    return newStr;
  }

  // otherwise the resource is empty just delete it and return an empty string
  delete resource;
  return Empty(nullptr);
}

Local<String> String::NewExternal(Isolate* isolate,
                                  ExternalOneByteStringResource *resource) {
  return FromMaybe(NewExternalOneByte(isolate, resource));
}

}  // namespace v8
