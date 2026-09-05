#include <type_traits>
#include <utility>
#include <sstream>
#include <string_view>
#include <iostream>
#include <ranges>
#include <meta>
#include <condition_variable>
#include <mutex>
#include <exception>
#include <stdexcept>
#include "jsonser.hh"
#include "cppgow/cppgowcxx.hh"

namespace Rest {
#define STRING_ANNOTATION(Name,builder)\
 template <std::size_t N> \
    struct Name {         \
        char data[N] {};  \
        consteval Name(const char (&s)[N]) { std::ranges::copy(s, s + N, data); } \
    };\
    template <std::size_t N> Name(const char (&)[N]) -> Name<N>;\
    template <std::size_t N> consteval Name<N> builder(\
        const char(&value)[N] ) {\
        return {value};\
    }\
    template <class T>\
    struct is##Name : std::false_type {};\
\
    template <std::size_t N>\
    struct is##Name<Name<N>> : std::true_type {};\
\
    template <class T>\
    inline constexpr bool is##Name##V = \
        is##Name<std::remove_cvref_t<T>>::value;

    STRING_ANNOTATION(RouteType, route)

    enum class InstanceModel: uint8_t
    {
        ThreadSafeSingleton,
        Transient,
        ThreadUnsafeWorkers,
    };
    extern thread_local const cppgow::ServerRequest* request;
    extern thread_local cppgow::ServerResponse* reply;

    template <typename T, typename = void>
    struct hasCast
            : std::false_type
    {
    };

    // Partial template specialisation, inherit from std::true_type.
    // ::value will return true.
    template <typename T>
    struct hasCast<T, std::void_t<decltype(std::declval<std::stringstream&>() >> std::declval<T&>())>>
            : std::true_type
    {
    };

    template<typename T, bool B=hasCast<T>::value> T cast(std::string const& src) {
         if constexpr (!B) {
            throw std::runtime_error("Cannot cast");
        } else if constexpr (std::is_same_v<T, std::string>) {
            return src;
        } else {
            std::stringstream ss(src);
            T res;
            ss >> res;
            return res;
        }
    }

    template<typename R>
    void invoke(std::function<R()> f, int pos, cppgow::ServerRequest const&request, cppgow::ServerResponse& reply)
    {
        auto res = f();
        Json::Value jv;
        jsonser::toJson(res, jv);
        std::stringstream ss;
        ss << jv;
        reply.headers["Content-Type"] = "application/json";
        reply.payload = cppgow::BufferView::copy(ss.str());
    }
    template<>
    inline void invoke(std::function<void()> f, int pos, cppgow::ServerRequest const&request, cppgow::ServerResponse& reply)
    {
        f();
    }
    template<typename R, typename A, typename...Rem>
    void invoke(std::function<R(A const&, Rem...)> f, int pos, cppgow::ServerRequest const&request, cppgow::ServerResponse& reply)
    {
        if (request.parameters.size() > pos)
            invoke((std::function<R(Rem...)>)[&](Rem... rem)->R {
                return f(cast<A>(request.parameters[pos]), rem...);}, pos+1, request, reply);
        else if (request.parameters.size() == pos)
            invoke((std::function<R(Rem...)>)[&](Rem... rem)->R {
                Json::Value jv;
                A a;
                std::stringstream ss(std::string((const char*)request.payload.data, ((const char*)request.payload.data)+request.payload.size));
                ss >> jv;
                jsonser::fromJson(a, jv);
                return f(a, rem...);},
                pos+1, request, reply);
        else
            throw std::runtime_error("failed to fill argument");
     }
    template<typename A, typename...Rem>
    void invoke(std::function<void(A const&, Rem...)> f, int pos, cppgow::ServerRequest const&request, cppgow::ServerResponse& reply)
    {
        if (request.parameters.size() > pos)
            invoke((std::function<void(Rem...)>)[&](Rem... rem)->void {
                f(cast<A>(request.parameters[pos]), rem...);}, pos+1, request, reply);
        else if (request.parameters.size() == pos)
            invoke((std::function<void(Rem...)>)[&](Rem... rem)->void {
                Json::Value jv;
                A a;
                std::stringstream ss(std::string((const char*)request.payload.data, ((const char*)request.payload.data)+request.payload.size));
                ss >> jv;
                jsonser::fromJson(a, jv);
                f(a, rem...);},
                pos+1, request, reply);
        else
            throw std::runtime_error("failed to fill argument");
    }
    template<typename T> class Singleton {
        static inline std::once_flag once;
        static inline std::shared_ptr<T> ptr;
        public:
        static std::shared_ptr<T> acquire() {
            std::call_once(once, [] { ptr = std::make_shared<T>();});
            return ptr;
        }
    };
    template<typename T> class WorkerPool {
        static inline std::once_flag once;
        static inline std::condition_variable cv;
        static inline std::mutex mut;
        static inline std::vector<std::shared_ptr<T>> available;
        public:
        static std::shared_ptr<T> acquire(int total) {
            std::call_once(once, [total]() {
                for (int i=0; i < total; i++) {
                    available.push_back(std::make_shared<T>());
                }
            });
            std::unique_lock lg(mut);
            while (available.empty())
                cv.wait(lg);
            auto res = available.back();
            available.pop_back();
            return res;
        }
        static void release(std::shared_ptr<T> ptr) {
            std::lock_guard lg(mut);
            available.push_back(ptr);
            if (available.size() == 1)
                cv.notify_one();
        }
    };
    template<typename T, typename R, typename...Args>
    cppgow::RouteHandler makeHandler(InstanceModel model, int nWorkers, R (T::*f)(Args...))
    {
        return [=](cppgow::ServerRequest const& request) {
            cppgow::ServerResponse reply;
            ::Rest::reply = &reply;
            ::Rest::request = &request;
            std::function<void ()> giveBack;
            std::shared_ptr<T> instance;
            try
            {
                reply.statusCode = 200;
                if (model == InstanceModel::ThreadSafeSingleton)
                    instance = Singleton<T>::acquire();
                else if (model == InstanceModel::Transient)
                    instance = std::make_shared<T>();
                else if (model == InstanceModel::ThreadUnsafeWorkers)
                {
                    instance = WorkerPool<T>::acquire(nWorkers);
                    giveBack = [instance]() { WorkerPool<T>::release(instance);};
                }
                if constexpr (std::is_same_v<R, void>)
                    invoke((std::function<void(Args...)>)[&](Args...args)->void { ((*instance).*f)(args...);}, 0, request, reply);
                else
                    invoke((std::function<R(Args...)>)[&](Args...args)->R {return ((*instance).*f)(args...);}, 0, request, reply);
            }
            catch (std::exception const& e)
            {
                reply.statusCode = 500;
                reply.payload = cppgow::BufferView::copy(e.what());
            }
            if (giveBack)
                giveBack();
            return reply;
        };
    }
    class RouteHandler
    {
        protected:
            cppgow::ServerRequest const& request() const { return *::Rest::request;}
            cppgow::ServerResponse& response() const { return *::Rest::reply;}
        public:
    };
    template<typename T>
    void registerRoutes(InstanceModel model, int nWorkers)
    {
        constexpr auto ctx = std::meta::access_context::unprivileged();
        constexpr static auto data_members = std::define_static_array(std::meta::members_of(^^T, ctx));
        template for (constexpr auto member : data_members) {
            if constexpr (std::meta::is_function(member)) {
                constexpr static auto anns = std::define_static_array(std::meta::annotations_of(member));
                template for (constexpr auto a : anns) {
                    using A = typename [: std::meta::type_of(a) :];
                    if constexpr (isRouteTypeV<A>) {
                        constexpr auto route = std::meta::extract<A>(a);
                        cppgow::registerRoute(route.data, makeHandler(model, nWorkers, &[:member:]));
                    }
                }
            }
        }
    }
}


#ifdef TEST
#include "unistd.h"
class Point: public jsonser::JsonSerializable
{
    public:
    int x;
    int y;
};
class Sample: public Rest::RouteHandler
{
    public:
    [[=Rest::route("/")]]
    int GetSlash()
    {
        return 42;
    }
    [[=Rest::route("/plusone/{value}")]]
    int PlusOne(int const& value)
    {
        return value + 1;
    }
    [[=Rest::route("/sum/{value}/{value2}")]]
    int Sum(int const& value, int const& value2)
    {
        return value + value2;
    }
    [[=Rest::route("/sumbody/{value}")]]
    int SumBody(int const& value, int const& value2)
    {
        return value + value2;
    }
    [[=Rest::route("/point")]]
    Point GetPoint()
    {
        return Point{.x=12, .y=40};
    }
    [[=Rest::route("/pointadd/{x}/{y}")]]
    Point PointAdd(int const&x, int const&y, Point const&p)
    {
        Point res {.x=p.x+x, .y=p.y+y};
        return res;
    }
    [[=Rest::route("/pointbad/{x}")]]
    Point Bad1(Point const&p)
    {
        Point res =p;
        return res;
    }
    [[=Rest::route("/delay/{x}")]]
    std::string Delay(int const& us)
    {
        response().headers["X-Waited-For"] = std::to_string(us);
        usleep(us);
        return "done";
    }
    [[=Rest::route("/tac/{val}")]]
    std::string Tac(std::string const& v)
    {
        return std::string(v.rbegin(), v.rend());
    }
};
int main()
{
    Rest::registerRoutes<Sample>(Rest::InstanceModel::ThreadUnsafeWorkers, 2);
    cppgow::listenAndServe("127.0.0.1:5566");
    while (true)
        usleep(1000000);
}
#endif