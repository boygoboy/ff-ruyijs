#include "document_model/exception_internal.h"
#include "document_model/exception_names.h"
#include "runtime/native_constructor_trace.h"
#include "jsapi.h"
#include "jsfriendapi.h"
#include "js/Exception.h"
#include "js/Object.h"
#include "js/PropertyAndElement.h"
#include "js/Realm.h"
#include <cstring>

namespace ruyijs::document_model {
// Slots: 0 name, 1 message, 2 legacy code, 3 subclass value (undefined for a
// plain DOMException; see CreateDerivedException), 4 filename, 5 line,
// 6 column, 7 captured stack — the location surface Firefox hangs off
// nsException (nsFrameMessageManager reads the same fields).
const JSClass kExceptionClass = {"DOMException", JSCLASS_HAS_RESERVED_SLOTS(8)};
bool ExceptionFields(JS::HandleObject object, JS::MutableHandleString name,
                      JS::MutableHandleString message) {
  if (JS::GetClass(object) != &kExceptionClass) return false;
  const auto label = JS::GetReservedSlot(object, 0), text = JS::GetReservedSlot(object, 1);
  if (!label.isString() || !text.isString()) return false;
  name.set(label.toString());message.set(text.toString());return true;
}
// Firefox stores DOMException text as UTF-8. Only unpaired surrogate units are
// replaced at that boundary; ordinary DOMString conversion remains unchanged.
static void Normalize(std::u16string& text) {
  for (size_t index = 0; index < text.size(); ++index) {
    const char16_t unit = text[index];
    if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < text.size() &&
        text[index + 1] >= 0xdc00 && text[index + 1] <= 0xdfff) { ++index;continue; }
    if (unit >= 0xd800 && unit <= 0xdfff) text[index] = 0xfffd;
  }
}
bool CreateException(JSContext* cx, JS::HandleObject installed, std::u16string_view message,
                      std::u16string_view name, JS::MutableHandleValue result, const JS::CallArgs* args) {
  JS::RootedObject prototype(cx, installed);
  if (args) {
    JS::RootedObject target(cx, &args->newTarget().toObject());
    JS::RootedValue value(cx);
    if (!JS_GetProperty(cx, target, "prototype", &value)) return false;
    if (value.isObject()) prototype = &value.toObject();
  }
  JS::RootedObject object(cx, JS_NewObjectWithGivenProto(cx, &kExceptionClass, prototype));
  JS::RootedValue label(cx), text(cx);
  if (!object || !Value(cx, name, &label) || !Value(cx, message, &text)) return false;
  int code = 0;
  for (size_t index = 0; index < std::size(kExceptionNames); ++index) {
    if (index == 1 || index == 5 || index == 15) continue;
    const char* ascii = kExceptionNames[index].name;
    if (name.size() != strlen(ascii)) continue;
    bool same = true;
    for (size_t unit = 0; unit < name.size(); ++unit) same &= name[unit] == ascii[unit];
    if (same) { code = int(index + 1);break; }
  }
  JS::SetReservedSlot(object, 0, label);JS::SetReservedSlot(object, 1, text);
  JS::SetReservedSlot(object, 2, JS::Int32Value(code));
  // The scripted frame that triggered the throw, exactly as Firefox reports it
  // through nsException's filename/lineNumber/columnNumber/stack accessors.
  JS::SetReservedSlot(object, 5, JS::Int32Value(0));
  JS::SetReservedSlot(object, 6, JS::Int32Value(0));
  {
    uint32_t line = 0;
    JS::ColumnNumberOneOrigin column;
    JS::AutoFilename file;
    if (JS::DescribeScriptedCaller(&file, cx, &line, &column)) {
      JS::RootedString fileString(cx, JS_NewStringCopyZ(cx, file.get() ? file.get() : ""));
      if (!fileString) return false;
      JS::SetReservedSlot(object, 4, JS::StringValue(fileString));
      JS::SetReservedSlot(object, 5, JS::Int32Value(int32_t(line)));
      JS::SetReservedSlot(object, 6, JS::Int32Value(int32_t(column.oneOriginValue())));
    }
    JS::RootedObject stack(cx);
    if (JS::CaptureCurrentStack(cx, &stack) && stack)
      JS::SetReservedSlot(object, 7, JS::ObjectValue(*stack));
  }
  result.setObject(*object);return true;
}
bool ThrowException(JSContext* cx, JS::HandleObject prototype, const char* name, const char* message) {
  std::u16string label, text;
  for (; *name; ++name) label.push_back(char16_t(static_cast<unsigned char>(*name)));
  for (; *message; ++message) text.push_back(char16_t(static_cast<unsigned char>(*message)));
  JS::RootedValue value(cx);
  {
    JSAutoRealm realm(cx, prototype);
    if (!CreateException(cx, prototype, text, label, &value)) return false;
  }
  if (!JS_WrapValue(cx, &value)) return false;
  JS_SetPendingException(cx, value);return false;
}
bool CreateDerivedException(JSContext* cx, JS::HandleObject prototype, JS::HandleString message,
                            const char* name, JS::HandleValue data, JS::MutableHandleValue result) {
  std::u16string text, label;
  JS::RootedValue raw(cx, JS::StringValue(message));
  if (!String(cx, raw, &text, false)) return false;
  Normalize(text);  // The same DOMString storage rule as the DOMException constructor.
  for (; *name; ++name) label.push_back(char16_t(static_cast<unsigned char>(*name)));
  if (!CreateException(cx, prototype, text, label, result)) return false;
  JS::SetReservedSlot(&result.toObject(), 3, data);
  return true;
}
bool DerivedExceptionData(JSObject* object, JS::MutableHandleValue data) {
  if (!object || JS::GetClass(object) != &kExceptionClass) return false;
  data.set(JS::GetReservedSlot(object, 3));
  return true;
}
bool ExceptionConstructor(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  NativeConstructorTrace trace(cx, args);
  if (!args.isConstructing()) return TypeError(cx, "DOMException requires new");
  std::u16string message, name = u"Error";
  if ((!args.get(0).isUndefined() && !String(cx, args[0], &message, false)) ||
      (!args.get(1).isUndefined() && !String(cx, args[1], &name, false))) return false;
  Normalize(message);Normalize(name);
  JS::RootedObject prototype(cx, &js::GetFunctionNativeReserved(&args.callee(), 0).toObject());
  return trace.Finish(CreateException(cx, prototype, message, name, args.rval(), &args));
}
bool ExceptionGetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  auto args = JS::CallArgsFromVp(argc, vp);
  if (!args.thisv().isObject() || JS::GetClass(&args.thisv().toObject()) != &kExceptionClass)
    return TypeError(cx, "DOMException receiver is incompatible");
  const auto slot = js::GetFunctionNativeReserved(&args.callee(), 1).toInt32();
  args.rval().set(JS::GetReservedSlot(&args.thisv().toObject(), slot));return true;
}
bool ExceptionReceiver(JSContext* cx, unsigned argc, JS::Value* vp, JS::MutableHandleObject self) {
  auto args = JS::CallArgsFromVp(argc, vp);
  if (!args.thisv().isObject() || JS::GetClass(&args.thisv().toObject()) != &kExceptionClass)
    return TypeError(cx, "DOMException receiver is incompatible");
  self.set(&args.thisv().toObject());return true;
}
// nsException's location accessors: empty string and 0 when the throw had no
// scripted caller (internal throws before any page frame).
bool ExceptionFilenameGetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  if (!ExceptionReceiver(cx, argc, vp, &self)) return false;
  const auto value = JS::GetReservedSlot(self, 4);
  args.rval().set(value.isString() ? value : JS::StringValue(JS_NewStringCopyZ(cx, "")));
  return true;
}
bool ExceptionLineGetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  if (!ExceptionReceiver(cx, argc, vp, &self)) return false;
  args.rval().set(JS::GetReservedSlot(self, 5));return true;
}
bool ExceptionColumnGetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  if (!ExceptionReceiver(cx, argc, vp, &self)) return false;
  args.rval().set(JS::GetReservedSlot(self, 6));return true;
}
// Error.prototype.stack formatting over the captured SavedFrame chain; the
// same string Firefox builds lazily for nsException.stack.
bool ExceptionStackGetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  if (!ExceptionReceiver(cx, argc, vp, &self)) return false;
  const auto saved = JS::GetReservedSlot(self, 7);
  JS::RootedString text(cx);
  if (saved.isObject()) {
    JS::RootedObject stack(cx, &saved.toObject());
    if (!JS::BuildStackString(cx, nullptr, stack, &text, 0, js::StackFormat::SpiderMonkey)) return false;
  } else if (!(text = JS_NewStringCopyZ(cx, ""))) return false;
  args.rval().setString(text);return true;
}
// NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_DOM(14), code): the legacy numeric
// result Firefox exposes as nsException.result (0x80530000 | code).
bool ExceptionResultGetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  if (!ExceptionReceiver(cx, argc, vp, &self)) return false;
  const auto code = JS::GetReservedSlot(self, 2);
  const unsigned result = 0x80530000u | (code.isInt32() ? unsigned(code.toInt32()) : 0u);
  args.rval().setNumber(double(result));return true;
}
// nsException.data: DOMException carries no exception payload; Firefox
// returns null here (nsIException::GetData).
bool ExceptionDataGetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  if (!ExceptionReceiver(cx, argc, vp, &self)) return false;
  args.rval().setNull();return true;
}
// [Replaceable] stack: an assignment defines an own data property on the
// receiver ({value, writable, enumerable, configurable}, the WebIDL
// [Replaceable] behaviour), leaving the captured frame chain untouched.
bool ExceptionStackSetter(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::RootedObject self(cx);
  if (!ExceptionReceiver(cx, argc, vp, &self)) return false;
  JS::RootedId key(cx);
  JS::RootedValue nameValue(cx, JS::StringValue(JS_NewStringCopyZ(cx, "stack")));
  if (!nameValue.isString() || !JS_ValueToId(cx, nameValue, &key)) return false;
  if (!JS_DefinePropertyById(cx, self, key, args.get(0),
                             JSPROP_ENUMERATE)) return false;
  args.rval().set(args.get(0));return true;
}
}
