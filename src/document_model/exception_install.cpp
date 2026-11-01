#include "document_model/exception_internal.h"
#include "document_model/exception_names.h"
#include "jsapi.h"
#include "jsfriendapi.h"
#include "js/PropertyAndElement.h"
#include "js/Realm.h"
#include "js/Symbol.h"

namespace ruyijs::document_model {
bool InstallExceptionSurface(JSContext* cx, JS::HandleObject global,
                              JS::MutableHandleObject prototype, JS::MutableHandleObject constructor) {
  JSAutoRealm realm(cx, global);
  JS::RootedObject error(cx);
  if (!JS_GetClassPrototype(cx, JSProto_Error, &error)) return false;
  prototype.set(JS_NewPlainObject(cx));
  JS::RootedFunction function(cx, js::NewFunctionWithReserved(cx, ExceptionConstructor, 0,
                                                            JSFUN_CONSTRUCTOR, "DOMException"));
  if (!prototype || !function || !JS_SetPrototype(cx, prototype, error)) return false;
  constructor.set(JS_GetFunctionObject(function));
  js::SetFunctionNativeReserved(constructor, 0, JS::ObjectValue(*prototype));
  JS::RootedString tag(cx, JS_NewStringCopyZ(cx, "DOMException"));
  JS::RootedId id(cx, JS::PropertyKey::Symbol(JS::GetWellKnownSymbol(cx, JS::SymbolCode::toStringTag)));
  if (!tag || !JS_DefineProperty(cx, constructor, "prototype", prototype, JSPROP_READONLY | JSPROP_PERMANENT) ||
      !JS_DefineProperty(cx, prototype, "constructor", constructor, 0) ||
      !JS_DefinePropertyById(cx, prototype, id, tag, JSPROP_READONLY) ||
      !JS_DefineProperty(cx, global, "DOMException", constructor, 0)) return false;
  const char* fields[] = {"name", "message", "code"};
  for (int32_t slot = 0; slot < 3; ++slot) {
    const std::string name = std::string("get ") + fields[slot];
    JS::RootedFunction getter(cx, js::NewFunctionWithReserved(cx, ExceptionGetter, 0, 0, name.c_str()));
    if (!getter) return false;
    JS::RootedObject get(cx, JS_GetFunctionObject(getter)), set(cx);
    js::SetFunctionNativeReserved(get, 1, JS::Int32Value(slot));
    if (!JS_DefineProperty(cx, prototype, fields[slot], get, set, JSPROP_ENUMERATE)) return false;
  }
  constexpr unsigned attrs = JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT;
  for (size_t index = 0; index < std::size(kExceptionNames); ++index) {
    JS::RootedValue value(cx, JS::Int32Value(int32_t(index + 1)));
    if (!JS_DefineProperty(cx, prototype, kExceptionNames[index].constant, value, attrs) ||
        !JS_DefineProperty(cx, constructor, kExceptionNames[index].constant, value, attrs)) return false;
  }
  // nsException's extra surface (the six members Firefox's DOMException
  // prototype carries beside name/message/code): the throw-site location, the
  // stack, the legacy nsresult, and the null payload accessor. Not enumerable,
  // exactly like Firefox's prototype accessors.
  struct ExceptionExtra { const char* name; bool (*native)(JSContext*, unsigned, JS::Value*); };
  constexpr ExceptionExtra extras[] = {
      {"filename", ExceptionFilenameGetter}, {"lineNumber", ExceptionLineGetter},
      {"columnNumber", ExceptionColumnGetter}, {"stack", ExceptionStackGetter},
      {"result", ExceptionResultGetter}, {"data", ExceptionDataGetter}};
  for (const auto& extra : extras) {
    JS::RootedFunction getter(cx, js::NewFunctionWithReserved(cx, extra.native, 0, 0, extra.name));
    if (!getter) return false;
    JS::RootedObject get(cx, JS_GetFunctionObject(getter)), set(cx);
    if (!JS_DefineProperty(cx, prototype, extra.name, get, set, JSPROP_READONLY)) return false;
  }
  return true;
}
}
