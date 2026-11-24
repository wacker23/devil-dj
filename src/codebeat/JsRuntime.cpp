#include "codebeat/JsRuntime.h"

#include "CodeBeatJs.h" // the embedded js/codebeat/*.js (juce_add_binary_data)

#include <quickjs.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <thread>

namespace devildj::codebeat
{

namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxPrinted = 1000;

// The library source for a module name, or nullptr. Only plain names of embedded files resolve: no paths.
const char* findModule(std::string_view name, int& size)
{
    if (name.starts_with("./"))
        name.remove_prefix(2);
    if (name.empty() || name.find_first_of("/\\:") != std::string_view::npos || name.find("..") != std::string_view::npos)
        return nullptr;
    for (int i = 0; i < CodeBeatJs::namedResourceListSize; ++i)
        if (name == CodeBeatJs::originalFilenames[i])
            return CodeBeatJs::getNamedResource(CodeBeatJs::namedResourceList[i], size);
    return nullptr;
}
} // namespace

struct JsRuntime::Impl
{
    JsLimits limits;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    std::thread::id owner = std::this_thread::get_id();
    Clock::time_point deadline = Clock::time_point::max();
    bool interrupted = false;
    std::deque<std::string> printed;

    explicit Impl(JsLimits l)
        : limits(l)
    {
        rt = JS_NewRuntime();
        JS_SetMemoryLimit(rt, limits.memoryBytes);
        JS_SetMaxStackSize(rt, limits.stackBytes);
        JS_SetInterruptHandler(rt, &Impl::onInterrupt, this);
        JS_SetModuleLoaderFunc(rt, nullptr, &Impl::loadModule, this);
        ctx = JS_NewContext(rt);
        JS_SetContextOpaque(ctx, this);
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, &Impl::print, "print", 1));
        JS_FreeValue(ctx, global);
    }

    ~Impl()
    {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    static int onInterrupt(JSContext*, void* opaque)
    {
        auto* self = static_cast<Impl*>(opaque);
        if (Clock::now() <= self->deadline)
            return 0;
        self->interrupted = true;
        return 1;
    }

    static JSModuleDef* loadModule(JSContext* ctx, const char* name, void*)
    {
        int size = 0;
        const char* source = findModule(name, size);
        if (source == nullptr)
        {
            JS_ThrowReferenceError(ctx, "no module named \"%s\" (only the Code Beat library can be imported)", name);
            return nullptr;
        }
        const std::string code(source, static_cast<std::size_t>(size)); // JS_Eval wants it zero-terminated
        JSValue compiled = JS_Eval(ctx, code.c_str(), code.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled))
            return nullptr;
        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JS_FreeValue(ctx, compiled);
        return module;
    }

    static JSValue print(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(ctx));
        std::string line;
        for (int i = 0; i < argc; ++i)
        {
            if (i > 0)
                line += ' ';
            if (const char* s = JS_ToCString(ctx, argv[i]))
            {
                line += s;
                JS_FreeCString(ctx, s);
            }
        }
        self->printed.push_back(std::move(line));
        while (self->printed.size() > kMaxPrinted)
            self->printed.pop_front();
        return JS_UNDEFINED;
    }

    std::string toString(JSValueConst v) const
    {
        std::size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        if (s == nullptr)
        {
            JS_FreeValue(ctx, JS_GetException(ctx)); // a value whose toString throws
            return "(a value that can't be shown)";
        }
        std::string out(s, len);
        JS_FreeCString(ctx, s);
        return out;
    }

    void begin(double budgetMs)
    {
        assert(std::this_thread::get_id() == owner && "JsRuntime is used on one thread only");
        JS_UpdateStackTop(rt); // the stack limit counts from here
        interrupted = false;
        deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(budgetMs));
    }

    void end() { deadline = Clock::time_point::max(); }

    // Where in `fileName` the error happened, from its stack ("at f (<code>:2:14)" / "at <code>:2:14").
    static void findLocation(const std::string& stack, std::string_view fileName, JsResult& r)
    {
        const std::string key = std::string(fileName) + ":";
        for (std::size_t at = stack.find(key); at != std::string::npos; at = stack.find(key, at + 1))
        {
            std::size_t i = at + key.size();
            int line = 0, column = 0;
            const auto digits = [&](int& out)
            {
                const std::size_t start = i;
                while (i < stack.size() && stack[i] >= '0' && stack[i] <= '9')
                    out = out * 10 + (stack[i++] - '0');
                return i > start;
            };
            if (!digits(line))
                continue;
            if (i < stack.size() && stack[i] == ':')
            {
                ++i;
                digits(column);
            }
            r.line = line;
            r.column = column;
            return;
        }
    }

    JsResult describe(JSValue exception, std::string_view fileName, double budgetMs)
    {
        JsResult r;
        if (interrupted)
        {
            r.timedOut = true;
            r.error = "too slow: stopped after " + std::to_string(static_cast<int>(budgetMs)) + " ms";
            return r;
        }
        r.error = toString(exception);
        r.outOfMemory = r.error.find("out of memory") != std::string::npos;
        if (JS_IsObject(exception))
        {
            JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
            if (JS_IsString(stack))
                findLocation(toString(stack), fileName, r);
            JS_FreeValue(ctx, stack);
        }
        if (r.outOfMemory)
            JS_RunGC(rt);
        return r;
    }

    JsResult finish(JSValue v, std::string_view fileName, double budgetMs)
    {
        JsResult r;
        if (JS_IsException(v))
        {
            JSValue exception = JS_GetException(ctx);
            r = describe(exception, fileName, budgetMs);
            JS_FreeValue(ctx, exception);
        }
        else
        {
            r.ok = true;
            r.value = toString(v);
        }
        JS_FreeValue(ctx, v);
        end();
        return r;
    }

    // Runs queued jobs (module evaluation settles through promises), inside the budget.
    bool runJobs()
    {
        JSContext* jobContext = nullptr;
        for (;;)
        {
            const int ran = JS_ExecutePendingJob(rt, &jobContext);
            if (ran == 0)
                return true;
            if (ran < 0)
            {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return false;
            }
        }
    }
};

JsRuntime::JsRuntime(JsLimits limits)
    : impl(std::make_unique<Impl>(limits))
{
}

JsRuntime::~JsRuntime() = default;

JsResult JsRuntime::evalScript(std::string_view code, std::string_view fileName, double budgetMs, std::string_view keepAs)
{
    const double budget = budgetMs > 0.0 ? budgetMs : impl->limits.evalMs;
    const std::string source(code), name(fileName);
    impl->begin(budget);
    JSValue v = JS_Eval(impl->ctx, source.c_str(), source.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (!keepAs.empty() && !JS_IsException(v))
    {
        JSValue global = JS_GetGlobalObject(impl->ctx);
        JS_SetPropertyStr(impl->ctx, global, std::string(keepAs).c_str(), JS_DupValue(impl->ctx, v));
        JS_FreeValue(impl->ctx, global);
    }
    return impl->finish(v, fileName, budget);
}

JsResult JsRuntime::evalModule(std::string_view code, std::string_view fileName, double budgetMs)
{
    const double budget = budgetMs > 0.0 ? budgetMs : impl->limits.evalMs;
    const std::string source(code), name(fileName);
    impl->begin(budget);
    JSValue v = JS_Eval(impl->ctx, source.c_str(), source.size(), name.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v) || !JS_IsPromise(v))
        return impl->finish(v, fileName, budget);
    // A module's evaluation is a promise: settle it, then report its error if it was rejected.
    impl->runJobs();
    const auto state = JS_PromiseState(impl->ctx, v);
    if (state == JS_PROMISE_REJECTED)
    {
        JSValue reason = JS_PromiseResult(impl->ctx, v);
        JS_FreeValue(impl->ctx, v);
        JsResult r = impl->describe(reason, fileName, budget);
        JS_FreeValue(impl->ctx, reason);
        impl->end();
        return r;
    }
    JS_FreeValue(impl->ctx, v);
    JsResult r;
    if (impl->interrupted)
    {
        r.timedOut = true;
        r.error = "too slow: stopped after " + std::to_string(static_cast<int>(budget)) + " ms";
    }
    else if (state == JS_PROMISE_PENDING)
    {
        r.error = "the module didn't finish (it is waiting on something that never comes)";
    }
    else
    {
        r.ok = true;
        r.value = "undefined";
    }
    impl->end();
    return r;
}

JsResult JsRuntime::callGlobal(std::string_view function, std::span<const double> args, double budgetMs)
{
    const double budget = budgetMs > 0.0 ? budgetMs : impl->limits.queryMs;
    auto* ctx = impl->ctx;
    impl->begin(budget);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, std::string(function).c_str());
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, fn))
    {
        JS_FreeValue(ctx, fn);
        impl->end();
        JsResult r;
        r.error = "no function called " + std::string(function);
        return r;
    }
    std::vector<JSValue> argv;
    argv.reserve(args.size());
    for (const double a : args)
        argv.push_back(JS_NewFloat64(ctx, a));
    JSValue v = JS_Call(ctx, fn, JS_UNDEFINED, static_cast<int>(argv.size()), argv.data());
    JS_FreeValue(ctx, fn);
    return impl->finish(v, "<code>", budget);
}

std::vector<std::string> JsRuntime::takePrinted()
{
    std::vector<std::string> out(impl->printed.begin(), impl->printed.end());
    impl->printed.clear();
    return out;
}

std::size_t JsRuntime::memoryUsed() const
{
    JSMemoryUsage usage{};
    JS_ComputeMemoryUsage(impl->rt, &usage);
    return static_cast<std::size_t>(usage.malloc_size);
}

const JsLimits& JsRuntime::limits() const noexcept
{
    return impl->limits;
}

std::vector<std::string> JsRuntime::libraryModules()
{
    std::vector<std::string> names;
    for (int i = 0; i < CodeBeatJs::namedResourceListSize; ++i)
        names.emplace_back(CodeBeatJs::originalFilenames[i]);
    return names;
}

} // namespace devildj::codebeat
