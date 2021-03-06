// Copyright (C) 2012 Bloomberg Finance L.P.

#include <v8.h>
#include <node.h>
#include <uv.h>

#include <blpapi_session.h>
#include <blpapi_eventdispatcher.h>

#include <blpapi_event.h>
#include <blpapi_message.h>
#include <blpapi_element.h>
#include <blpapi_name.h>
#include <blpapi_request.h>
#include <blpapi_subscriptionlist.h>
#include <blpapi_defs.h>

#include <deque>
#include <sstream>
#include <vector>

#include <cmath>
#include <ctime>
#include <cstdlib>

#define BLPAPI_EXCEPTION_TRY try {
#define BLPAPI_EXCEPTION_CATCH \
    } catch (blpapi::Exception& e) { \
        ThrowException(Exception::Error( \
                String::New(e.description().c_str(), \
                            e.description().length()))); \
    }
#define BLPAPI_EXCEPTION_CATCH_RETURN \
    } catch (blpapi::Exception& e) { \
        return ThrowException(Exception::Error( \
                String::New(e.description().c_str(), \
                            e.description().length()))); \
    }

using namespace node;
using namespace v8;

namespace BloombergLP {
namespace blpapijs {

class Session : public ObjectWrap,
                public blpapi::EventHandler {
public:
    Session(const char *host, int port);
    ~Session();

    static void Initialize(Handle<Object> target);
    static Handle<Value> New(const Arguments& args);

    static Handle<Value> Start(const Arguments& args);
    static Handle<Value> Stop(const Arguments& args);
    static Handle<Value> Destroy(const Arguments& args);
    static Handle<Value> OpenService(const Arguments& args);
    static Handle<Value> Subscribe(const Arguments& args);
    static Handle<Value> Resubscribe(const Arguments& args);
    static Handle<Value> Request(const Arguments& args);

private:
    Session();
    Session(const Session&);
    Session& operator=(const Session&);

    static Handle<Value> subscribe(const Arguments& args, bool resubscribe);
    static void formFields(std::string* str, Handle<Object> array);
    static void formOptions(std::string* str, Handle<Value> array);
    static Handle<Value> elementToValue(const blpapi::Element& e);
    static Handle<Value> elementValueToValue(const blpapi::Element& e,
                                             int idx = 0);

    bool processEvent(const blpapi::Event& ev, blpapi::Session* session);
    static void processEvents(uv_async_t *async, int status);
    void processMessage(blpapi::Event::EventType et,
                        const blpapi::Message& msg);

    void emit(int argc, Handle<Value> argv[]);

    static uv_async_t s_async;
    static Persistent<String> s_emit;
    static Persistent<String> s_event_type;
    static Persistent<String> s_message_type;
    static Persistent<String> s_topic_name;
    static Persistent<String> s_correlations;
    static Persistent<String> s_value;
    static Persistent<String> s_class_id;
    static Persistent<String> s_data;

    blpapi::SessionOptions d_options;
    blpapi::Session *d_session;
    Persistent<Object> d_session_ref;
    std::deque<blpapi::Event> d_que;
    pthread_mutex_t d_que_mutex;
    bool d_started;
    bool d_stopped;
};

uv_async_t Session::s_async;
Persistent<String> Session::s_emit;
Persistent<String> Session::s_event_type;
Persistent<String> Session::s_message_type;
Persistent<String> Session::s_topic_name;
Persistent<String> Session::s_correlations;
Persistent<String> Session::s_value;
Persistent<String> Session::s_class_id;
Persistent<String> Session::s_data;

Session::Session(const char *host, int port)
    : d_started(false)
    , d_stopped(false)
{
    d_options.setServerHost(host);
    d_options.setServerPort(port);

    BLPAPI_EXCEPTION_TRY
    d_session = new blpapi::Session(d_options, this);
    BLPAPI_EXCEPTION_CATCH

    pthread_mutex_init(&d_que_mutex, NULL);

    uv_ref(uv_default_loop());
}

Session::~Session()
{
    // Ref on the event loop is released in Destroy

    pthread_mutex_destroy(&d_que_mutex);
}

void
Session::Initialize(Handle<Object> target)
{
    HandleScope scope;
    Local<FunctionTemplate> t = FunctionTemplate::New(Session::New);

    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "start", Start);
    NODE_SET_PROTOTYPE_METHOD(t, "stop", Stop);
    NODE_SET_PROTOTYPE_METHOD(t, "destroy", Destroy);
    NODE_SET_PROTOTYPE_METHOD(t, "openService", OpenService);
    NODE_SET_PROTOTYPE_METHOD(t, "subscribe", Subscribe);
    NODE_SET_PROTOTYPE_METHOD(t, "resubscribe", Resubscribe);
    NODE_SET_PROTOTYPE_METHOD(t, "request", Request);

    target->Set(String::NewSymbol("Session"), t->GetFunction());

    uv_async_init(uv_default_loop(), &s_async, Session::processEvents);
    uv_unref(uv_default_loop());

    s_emit = NODE_PSYMBOL("emit");
    s_event_type = NODE_PSYMBOL("eventType");
    s_message_type = NODE_PSYMBOL("messageType");
    s_topic_name = NODE_PSYMBOL("topicName");
    s_correlations = NODE_PSYMBOL("correlations");
    s_value = NODE_PSYMBOL("value");
    s_class_id = NODE_PSYMBOL("classId");
    s_data = NODE_PSYMBOL("data");
}

Handle<Value>
Session::New(const Arguments& args)
{
    HandleScope scope;

    char host[128] = "";
    int port = 0;

    if (args.Length() > 0 && args[0]->IsObject()) {
        Local<Object> o = args[0]->ToObject();

        // Capture the host name
        Local<Value> h = o->Get(String::New("host"));
        if (h->IsString()) {
            h->ToString()->WriteAscii(host, 0, sizeof(host));
            host[sizeof(host)-1] = '\0';
        }
        if (0 == host[0])
            return ThrowException(Exception::Error(String::New(
                        "Configuration missing 'host'.")));

        // Capture the port number
        Local<Value> p = o->Get(String::New("port"));
        if (p->IsInt32())
            port = p->ToInt32()->Value();
        if (0 == port)
            return ThrowException(Exception::Error(String::New(
                        "Configuration missing non-zero 'port'.")));
    } else {
        return ThrowException(Exception::Error(String::New(
                        "Configuration object must be passed as parameter.")));
    }

    Session *session = new Session(host, port);
    session->Wrap(args.This());
    return scope.Close(args.This());
}

Handle<Value>
Session::Start(const Arguments& args)
{
    HandleScope scope;

    Session* session = ObjectWrap::Unwrap<Session>(args.This());

    if (session->d_started)
        return ThrowException(Exception::Error(String::New(
                        "Session has already been started.")));
    if (session->d_stopped)
        return ThrowException(Exception::Error(String::New(
                        "Stopped sessions can not be restarted.")));

    BLPAPI_EXCEPTION_TRY
    session->d_session->startAsync();
    BLPAPI_EXCEPTION_CATCH_RETURN

    session->d_session_ref = Persistent<Object>::New(args.This());
    session->d_started = true;

    return scope.Close(args.This());
}

Handle<Value>
Session::Stop(const Arguments& args)
{
    HandleScope scope;

    Session* session = ObjectWrap::Unwrap<Session>(args.This());

    if (!session->d_started)
        return ThrowException(Exception::Error(String::New(
                        "Session has not been started.")));
    if (session->d_stopped)
        return ThrowException(Exception::Error(String::New(
                        "Session has already been stopped.")));

    session->d_stopped = true;

    BLPAPI_EXCEPTION_TRY
    session->d_session->stopAsync();
    BLPAPI_EXCEPTION_CATCH_RETURN

    return scope.Close(args.This());
}

Handle<Value>
Session::Destroy(const Arguments& args)
{
    HandleScope scope;

    Session* session = ObjectWrap::Unwrap<Session>(args.This());

    if (!session->d_started)
        return ThrowException(Exception::Error(String::New(
                        "Session has not been started.")));
    if (!session->d_stopped)
        return ThrowException(Exception::Error(String::New(
                        "Session has not been stopped.")));

    session->d_session_ref.Dispose();

    uv_unref(uv_default_loop());

    return scope.Close(args.This());
}

Handle<Value>
Session::OpenService(const Arguments& args)
{
    HandleScope scope;

    if (args.Length() < 1 || !args[0]->IsString()) {
        return ThrowException(Exception::Error(String::New(
                "Service URI string must be provided as first parameter.")));
    }
    if (args.Length() < 2 || !args[1]->IsInt32()) {
        return ThrowException(Exception::Error(String::New(
                "Integer correlation identifier must be provided "
                "as second parameter.")));
    }
    if (args.Length() > 2) {
        return ThrowException(Exception::Error(String::New(
                "Function expects at most two arguments.")));
    }

    Local<String> s = args[0]->ToString();
    std::vector<char> uriv;
    uriv.reserve(s->Utf8Length() + 1);
    s->WriteUtf8(&uriv[0]);

    int cidi = args[1]->Int32Value();
    blpapi::CorrelationId cid(cidi);

    Session* session = ObjectWrap::Unwrap<Session>(args.This());

    BLPAPI_EXCEPTION_TRY
    session->d_session->openServiceAsync(&uriv[0], cid);
    BLPAPI_EXCEPTION_CATCH_RETURN

    return scope.Close(Integer::New(cidi));
}

void
Session::formFields(std::string* str, Handle<Object> object)
{
    // Use the HandleScope of the calling function for speed.

    assert(object->IsArray());

    std::stringstream ss;

    // Format each array value into the options string "V[&V]"
    for (int i = 0; i < Array::Cast(*object)->Length(); ++i) {
        Local<String> s = object->Get(i)->ToString();
        std::vector<char> v;
        v.reserve(s->Utf8Length() + 1);
        s->WriteUtf8(&v[0]);
        if (i > 0)
            ss << ",";
        ss << &v[0];
    }

    *str = ss.str();
}

void
Session::formOptions(std::string* str, Handle<Value> value)
{
    // Use the HandleScope of the calling function for speed.

    if (value->IsUndefined() || value->IsNull())
        return;

    assert(value->IsObject());

    std::stringstream ss;

    if (value->IsArray()) {
        // Format each array value into the options string "V[&V]"
        Local<Object> object = value->ToObject();
        for (int i = 0; i < Array::Cast(*object)->Length(); ++i) {
            Local<String> key = object->Get(i)->ToString();
            std::vector<char> valv;
            valv.reserve(key->Utf8Length() + 1);
            key->WriteUtf8(&valv[0]);
            if (i > 0)
                ss << "&";
            ss << &valv[0];
        }
    } else {
        // Format each KV pair into the options string "K=V[&K=V]"
        Local<Object> object = value->ToObject();
        Local<Array> keys = object->GetPropertyNames();
        for (int i = 0; i < keys->Length(); ++i) {
            Local<String> key = keys->Get(i)->ToString();
            std::vector<char> keyv;
            keyv.reserve(key->Utf8Length() + 1);
            key->WriteUtf8(&keyv[0]);
            if (i > 0)
                ss << "&";
            ss << &keyv[0] << "=";

            Local<String> val = object->Get(key)->ToString();
            std::vector<char> valv;
            valv.reserve(val->Utf8Length() + 1);
            val->WriteUtf8(&valv[0]);
            ss << &valv[0];
        }
    }

    *str = ss.str();
}

Handle<Value>
Session::subscribe(const Arguments& args, bool resubscribe)
{
    HandleScope scope;

    if (args.Length() < 1 || !args[0]->IsArray()) {
        return ThrowException(Exception::Error(String::New(
                "Array of subscription information must be provided.")));
    }
    if (args.Length() >= 2 && !args[1]->IsUndefined() && !args[1]->IsString()) {
        return ThrowException(Exception::Error(String::New(
                "Optional subscription label must be a string.")));
    }
    if (args.Length() > 2) {
        return ThrowException(Exception::Error(String::New(
                "Function expects at most two arguments.")));
    }

    blpapi::SubscriptionList sl;

    Local<Object> o = args[0]->ToObject();
    for (int i = 0; i < Array::Cast(*(args[0]))->Length(); ++i) {
        Local<Value> v = o->Get(i);
        if (!v->IsObject()) {
            return ThrowException(Exception::Error(String::New(
                        "Array elements must be objects "
                        "containing subscription information.")));
        }
        Local<Object> io = v->ToObject();

        // Process 'security' string
        Local<Value> iv = io->Get(String::New("security"));
        if (!iv->IsString()) {
            return ThrowException(Exception::Error(String::New(
                        "Property 'security' must be a string.")));
        }
        std::vector<char> secv;
        secv.reserve(iv->ToString()->Length() + 1);
        iv->ToString()->WriteUtf8(&secv[0]);

        // Process 'fields' array
        iv = io->Get(String::New("fields"));
        if (!iv->IsArray()) {
            return ThrowException(Exception::Error(String::New(
                        "Property 'fields' must be an array of strings.")));
        }
        std::string fields;
        formFields(&fields, iv->ToObject());

        // Process 'options' array
        iv = io->Get(String::New("options"));
        if (!iv->IsUndefined() && !iv->IsNull() && !iv->IsObject()) {
            return ThrowException(Exception::Error(String::New(
                        "Property 'options' must be an object containing "
                        "whose keys and key values will be configured as "
                        "options.")));
        }
        std::string options;
        formOptions(&options, iv);

        // Process 'correlation' int or string
        iv = io->Get(String::New("correlation"));
        if (!iv->IsInt32()) {
            return ThrowException(Exception::Error(String::New(
                        "Property 'correlation' must be an integer.")));
        }
        int correlation = iv->Int32Value();

        sl.add(&secv[0], fields.c_str(), options.c_str(),
               blpapi::CorrelationId(correlation));
    }

    Session* session = ObjectWrap::Unwrap<Session>(args.This());

    BLPAPI_EXCEPTION_TRY
    if (args.Length() == 2) {
        Local<String> s = args[1]->ToString();
        std::vector<char> labelv;
        labelv.reserve(s->Length() + 1);
        s->WriteUtf8(&labelv[0]);
        if (resubscribe)
            session->d_session->resubscribe(sl, &labelv[0], labelv.size());
        else
            session->d_session->subscribe(sl, &labelv[0], labelv.size());
    } else {
        if (resubscribe)
            session->d_session->resubscribe(sl);
        else
            session->d_session->subscribe(sl);
    }
    BLPAPI_EXCEPTION_CATCH_RETURN

    return scope.Close(args.This());
}

Handle<Value>
Session::Subscribe(const Arguments& args)
{
    return Session::subscribe(args, false);
}

Handle<Value>
Session::Resubscribe(const Arguments& args)
{
    return Session::subscribe(args, true);
}

static inline void
mkdatetime(blpapi::Datetime* dt, Local<Value> val)
{
    double ms = Date::Cast(*val)->NumberValue();
    time_t sec = static_cast<time_t>(ms / 1000.0);
    int remainder = static_cast<int>(fmod(ms, 1000.0));

    struct tm tm;
    gmtime_r(&sec, &tm);

    dt->setDate(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    dt->setTime(tm.tm_hour, tm.tm_min, tm.tm_sec, remainder);
}

Handle<Value>
Session::Request(const Arguments& args)
{
    HandleScope scope;

    if (args.Length() < 1 || !args[0]->IsString()) {
        return ThrowException(Exception::Error(String::New(
                "Service URI string must be provided as first parameter.")));
    }
    if (args.Length() < 2 || !args[1]->IsString()) {
        return ThrowException(Exception::Error(String::New(
                "String request name must be provided as second parameter.")));
    }
    if (args.Length() < 3 || !args[2]->IsObject()) {
        return ThrowException(Exception::Error(String::New(
                "Object containing request parameters must be provided "
                "as third parameter.")));
    }
    if (args.Length() < 4 || !args[3]->IsInt32()) {
        return ThrowException(Exception::Error(String::New(
                "Integer correlation identifier must be provided "
                "as fourth parameter.")));
    }
    if (args.Length() >= 5 && !args[4]->IsUndefined() && !args[4]->IsString()) {
        return ThrowException(Exception::Error(String::New(
                "Optional request label must be a string.")));
    }
    if (args.Length() > 5) {
        return ThrowException(Exception::Error(String::New(
                "Function expects at most five arguments.")));
    }

    int cidi = args[3]->Int32Value();

    Session* session = ObjectWrap::Unwrap<Session>(args.This());

    BLPAPI_EXCEPTION_TRY

    Local<String> uri = args[0]->ToString();
    std::vector<char> uriv;
    uriv.reserve(uri->Length() + 1);
    uri->WriteAscii(&uriv[0]);

    blpapi::Service service = session->d_session->getService(&uriv[0]);

    Local<String> name = args[1]->ToString();
    std::vector<char> namev;
    namev.reserve(name->Utf8Length() + 1);
    name->WriteUtf8(&namev[0]);

    blpapi::Request request = service.createRequest(&namev[0]);

    // Loop over object properties, appending/setting into the request.
    Local<Object> obj = args[2]->ToObject();
    Local<Array> props = obj->GetPropertyNames();

    for (int i = 0; i < props->Length(); ++i) {
        // Process the key.
        Local<Value> keyval = props->Get(i);
        Local<String> key = keyval->ToString();
        std::vector<char> keyv;
        keyv.reserve(key->Utf8Length() + 1);
        key->WriteUtf8(&keyv[0]);

        // Process the value.
        //
        // The values present on the outer object are marshalled into the
        // blpapi::Request by setting values using 'set'.  Arrays indicate
        // values which should be marshalled using 'append'.
        Local<Value> val = obj->Get(keyval);
        if (val->IsString()) {
            Local<String> s = val->ToString();
            std::vector<char> valv;
            valv.reserve(s->Utf8Length() + 1);
            s->WriteUtf8(&valv[0]);
            request.set(&keyv[0], &valv[0]);
        } else if (val->IsBoolean()) {
            request.set(&keyv[0], val->BooleanValue());
        } else if (val->IsNumber()) {
            request.set(&keyv[0], val->NumberValue());
        } else if (val->IsInt32()) {
            request.set(&keyv[0], val->Int32Value());
        } else if (val->IsUint32()) {
            request.set(&keyv[0],
                static_cast<blpapi::Int64>(val->Uint32Value()));
        } else if (val->IsDate()) {
            blpapi::Datetime dt;
            mkdatetime(&dt, val);
            request.set(&keyv[0], dt);
        } else if (val->IsArray()) {
            // Arrays are marshalled into the blpapi::Request by appending
            // value types using the key of the array in the outer object.
            Local<Object> subarray = val->ToObject();
            int jmax = Array::Cast(*val)->Length();
            for (int j = 0; j < jmax; ++j) {
                Local<Value> subval = subarray->Get(j);
                // Only strings, booleans, and numbers are marshalled.
                if (subval->IsString()) {
                    Local<String> s = subval->ToString();
                    std::vector<char> subvalv;
                    subvalv.reserve(s->Utf8Length() + 1);
                    s->WriteUtf8(&subvalv[0]);
                    request.append(&keyv[0], &subvalv[0]);
                } else if (subval->IsBoolean()) {
                    request.append(&keyv[0], subval->BooleanValue());
                } else if (subval->IsNumber()) {
                    request.append(&keyv[0], subval->NumberValue());
                } else if (subval->IsInt32()) {
                    request.append(&keyv[0], subval->Int32Value());
                } else if (subval->IsUint32()) {
                    request.append(&keyv[0],
                        static_cast<blpapi::Int64>(subval->Uint32Value()));
                } else if (subval->IsDate()) {
                    blpapi::Datetime dt;
                    mkdatetime(&dt, subval);
                    request.append(&keyv[0], dt);
                } else {
                    return ThrowException(Exception::Error(String::New(
                                "Array contains invalid value type.")));
                }
            }
        } else {
            return ThrowException(Exception::Error(String::New(
                        "Object contains invalid value type.")));
        }
    }

    blpapi::CorrelationId cid(cidi);

    if (args.Length() == 5) {
        std::vector<char> labelv;
        Local<String> s = args[4]->ToString();
        labelv.reserve(s->Utf8Length() + 1);
        int len = s->WriteUtf8(&labelv[0]);
        session->d_session->sendRequest(request, cid, 0, &labelv[0], len);
    } else {
        session->d_session->sendRequest(request, cid);
    }

    BLPAPI_EXCEPTION_CATCH_RETURN

    return scope.Close(Integer::New(cidi));
}

Handle<Value>
Session::elementToValue(const blpapi::Element& e)
{
    if (e.isComplexType()) {
        int numElements = e.numElements();
        Local<Object> o = Object::New();
        for (int i = 0; i < numElements; ++i) {
            blpapi::Element se = e.getElement(i);
            Handle<Value> sev;
            if (se.isComplexType() || se.isArray()) {
                sev = elementToValue(se);
            } else {
                sev = elementValueToValue(se);
            }
            o->Set(String::New(se.name().string(), se.name().length()),
                   sev, (PropertyAttribute)(ReadOnly | DontDelete));
        }
        return o;
    } else if (e.isArray()) {
        int numValues = e.numValues();
        Local<Object> o = Array::New(numValues);
        for (int i = 0; i < numValues; ++i) {
            o->Set(i, elementValueToValue(e, i));
        }
        return o;
    } else {
        return elementValueToValue(e);
    }
}

static inline struct tm*
mknow(struct tm* tm)
{
    time_t sec;
    time(&sec);
    struct tm* ret = gmtime_r(&sec, tm);
    return ret;
}

static inline time_t
mkutctime(struct tm* tm)
{
    time_t ret;
    char* tz = getenv("TZ");
    setenv("TZ", "UTC", 1);
    tzset();
    ret = mktime(tm);
    if (tz)
        setenv("TZ", tz, 1);
    else
        unsetenv("TZ");
    tzset();
    return ret;
}

Handle<Value>
Session::elementValueToValue(const blpapi::Element& e, int idx)
{
    if (e.isNull())
        return Null();

    switch (e.datatype()) {
        case blpapi::DataType::BOOL:
            return Boolean::New(e.getValueAsBool(idx));
        case blpapi::DataType::CHAR: {
            char c = e.getValueAsChar(idx);
            return String::New(&c, 1);
        }
        case blpapi::DataType::BYTE:
        case blpapi::DataType::INT32:
            return Integer::New(e.getValueAsInt32(idx));
        case blpapi::DataType::FLOAT32:
            return Number::New(e.getValueAsFloat32(idx));
        case blpapi::DataType::FLOAT64:
            return Number::New(e.getValueAsFloat64(idx));
        case blpapi::DataType::ENUMERATION: {
            blpapi::Name n = e.getValueAsName(idx);
            return String::New(n.string(), n.length());
        }
        case blpapi::DataType::INT64: {
            // IEEE754 double can represent the range [-2^53, 2^53].
            static const blpapi::Int64 MAX_DOUBLE_INT = 9007199254740992LL;
            blpapi::Int64 i = e.getValueAsInt64(idx);
            if ((i >= -MAX_DOUBLE_INT) && (i <= MAX_DOUBLE_INT))
                return Number::New(static_cast<double>(i));
            break;
        }
        case blpapi::DataType::STRING:
            return String::New(e.getValueAsString(idx));
        case blpapi::DataType::DATE: {
            blpapi::Datetime dt = e.getValueAsDatetime(idx);
            if (dt.hasParts(blpapi::DatetimeParts::DATE)) {
                struct tm date;
                date.tm_sec = 0;
                date.tm_min = 0;
                date.tm_hour = 0;
                date.tm_mday = dt.day();
                date.tm_mon = dt.month() - 1;
                date.tm_year = dt.year() - 1900;
                date.tm_wday = 0;
                date.tm_yday = 0;
                date.tm_isdst = 0;
                time_t sec = mkutctime(&date);
                double ms = sec * 1000.0L;
                return Date::New(ms);
            }
            break;
        }
        case blpapi::DataType::TIME: {
            blpapi::Datetime dt = e.getValueAsDatetime(idx);
            if (dt.hasParts(blpapi::DatetimeParts::TIME)) {
                struct tm date;
                mknow(&date);
                date.tm_sec = dt.seconds();
                date.tm_min = dt.minutes();
                date.tm_hour = dt.hours();
                date.tm_isdst = 0;
                time_t sec = mkutctime(&date);
                double ms = sec * 1000.0L;
                if (dt.hasParts(blpapi::DatetimeParts::TIMEMILLI))
                    ms += dt.milliSeconds();
                return Date::New(ms);
            }
            break;
        }
        case blpapi::DataType::DATETIME: {
            blpapi::Datetime dt = e.getValueAsDatetime(idx);
            struct tm date;
            // Use date if present, otherwise default to "now".
            if (dt.hasParts(blpapi::DatetimeParts::DATE)) {
                date.tm_mday = dt.day();
                date.tm_mon = dt.month() - 1;
                date.tm_year = dt.year() - 1900;
                date.tm_wday = 0;
                date.tm_yday = 0;
            } else {
                mknow(&date);
            }
            // Use time if present, otherwise default to midnight.
            if (dt.hasParts(blpapi::DatetimeParts::TIME)) {
                date.tm_sec = dt.seconds();
                date.tm_min = dt.minutes();
                date.tm_hour = dt.hours();
                date.tm_isdst = 0;
            } else {
                date.tm_sec = 0;
                date.tm_min = 0;
                date.tm_hour = 0;
                date.tm_isdst = 0;
            }
            time_t sec = mkutctime(&date);
            double ms = sec * 1000.0L;
            if (dt.hasParts(blpapi::DatetimeParts::TIMEMILLI))
                ms += dt.milliSeconds();
            return Date::New(ms);
        }
        case blpapi::DataType::SEQUENCE:
            return elementToValue(e.getValueAsElement(idx));
        default:
            break;
    }

    return Null();
}

class StaticStringResource : public String::ExternalAsciiStringResource
{
  public:
    StaticStringResource(const char* str)
        : d_str(str) { d_len = strlen(d_str); }
    StaticStringResource(const StaticStringResource& rhs)
        : d_str(rhs.d_str), d_len(rhs.d_len) {}
    ~StaticStringResource() {}

    StaticStringResource& operator=(const StaticStringResource& rhs) {
        d_str = rhs.d_str;
        d_len = rhs.d_len;
        return *this;
    }

    const char* data() const { return d_str; }
    size_t length() const { return d_len; }

  private:
    const char* d_str;
    size_t d_len;
};

#define EVENT_TO_STRING(e) \
    case blpapi::Event::e : \
        static StaticStringResource s_##e(#e); \
        return String::NewExternal(new StaticStringResource(s_##e))

static inline Handle<Value>
eventTypeToString(blpapi::Event::EventType et)
{
    switch (et) {
        EVENT_TO_STRING(ADMIN);
        EVENT_TO_STRING(SESSION_STATUS);
        EVENT_TO_STRING(SUBSCRIPTION_STATUS);
        EVENT_TO_STRING(REQUEST_STATUS);
        EVENT_TO_STRING(RESPONSE);
        EVENT_TO_STRING(PARTIAL_RESPONSE);
        EVENT_TO_STRING(SUBSCRIPTION_DATA);
        EVENT_TO_STRING(SERVICE_STATUS);
        EVENT_TO_STRING(TIMEOUT);
        EVENT_TO_STRING(AUTHORIZATION_STATUS);
        EVENT_TO_STRING(RESOLUTION_STATUS);
        EVENT_TO_STRING(TOPIC_STATUS);
        EVENT_TO_STRING(TOKEN_STATUS);
        EVENT_TO_STRING(REQUEST);
        EVENT_TO_STRING(UNKNOWN);
    }
}

void
Session::processMessage(blpapi::Event::EventType et, const blpapi::Message& msg)
{
    Handle<Value> argv[2];

    const blpapi::Name& messageType = msg.messageType();
    argv[0] = String::New(messageType.string(), messageType.length());

    Local<Object> o = Object::New();

    o->Set(s_event_type, eventTypeToString(et),
           (PropertyAttribute)(ReadOnly | DontDelete));
    o->Set(s_message_type, argv[0],
           (PropertyAttribute)(ReadOnly | DontDelete));
    o->Set(s_topic_name, String::New(msg.topicName()),
           (PropertyAttribute)(ReadOnly | DontDelete));

    Local<Array> correlations = Array::New(msg.numCorrelationIds());
    for (int i = 0, j = 0; i < msg.numCorrelationIds(); ++i) {
        blpapi::CorrelationId cid = msg.correlationId(i);
        // Only pack user-specified integers and auto-generated
        // values into the correlations array returned to the user.
        if (cid.valueType() == blpapi::CorrelationId::INT_VALUE ||
            cid.valueType() == blpapi::CorrelationId::AUTOGEN_VALUE) {
            Local<Object> cido = Object::New();
            cido->Set(s_value, Integer::New(cid.asInteger()));
            cido->Set(s_class_id, Integer::New(cid.classId()));
            correlations->Set(j++, cido);
        } else {
            correlations->Set(j++, Object::New());
        }
    }
    o->Set(s_correlations, correlations,
           (PropertyAttribute)(ReadOnly | DontDelete));

    o->Set(s_data, elementToValue(msg.asElement()));

    argv[1] = o;

    this->emit(ARRAY_SIZE(argv), argv);
}

void
Session::processEvents(uv_async_t *async, int status)
{
    HandleScope scope;

    Session *session = reinterpret_cast<Session *>(async->data);

    bool empty;
    do {
        // Determine if the queue is empty
        pthread_mutex_lock(&session->d_que_mutex);
        empty = session->d_que.empty();
        if (empty) {
            pthread_mutex_unlock(&session->d_que_mutex);
            break;
        }

        // Keep the lock and release once the head is retrieved
        const blpapi::Event& ev = session->d_que.front();
        pthread_mutex_unlock(&session->d_que_mutex);

        // Iterate over contained messages without holding lock
        blpapi::MessageIterator msgIter(ev);
        while (msgIter.next()) {
            const blpapi::Message& msg = msgIter.message();
            session->processMessage(ev.eventType(), msg);
        }

        // Reacquire and pop, updating empty flag to having to
        // loop and acquire the mutex for a second time.
        pthread_mutex_lock(&session->d_que_mutex);
        session->d_que.pop_front();
        empty = session->d_que.empty();
        pthread_mutex_unlock(&session->d_que_mutex);
    } while (!empty);
}

bool
Session::processEvent(const blpapi::Event& ev, blpapi::Session* session)
{
    pthread_mutex_lock(&d_que_mutex);

    d_que.push_back(ev);

    pthread_mutex_unlock(&d_que_mutex);

    s_async.data = this;
    uv_async_send(&s_async);

    return true;
}

void
Session::emit(int argc, Handle<Value> argv[])
{
    HandleScope scope;

    Local<Function> emit = Local<Function>::Cast(handle_->Get(s_emit));
    emit->Call(handle_, argc, argv);
}

}   // close namespace blpapijs
}   // close namespace BloombergLP

extern "C" {
    void NODE_EXTERN init(Handle<Object> target) {
        BloombergLP::blpapijs::Session::Initialize(target);
    }
    NODE_MODULE(blpapi, init);
}
