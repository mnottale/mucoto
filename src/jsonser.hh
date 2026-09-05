#include <type_traits>
#include <string_view>
#include <iostream>
#include <ranges>
#include <meta>
#include <exception>
#include <stdexcept>
#include <json/value.h>
#include <json/writer.h>
#include <json/reader.h>

using std::meta::access_context;
using std::convertible_to;
using std::remove_cvref_t;
using std::string;
using std::string_view;

namespace jsonser
{
    // Define a marker annotation named Exclude,
    // with a unique, anonymous, empty struct
    struct ExcludeTag {};
    inline constexpr ExcludeTag Exclude{};
    struct RequiredTag {};
    inline constexpr RequiredTag Required{};
    template <std::size_t N>
    struct Name {
        char data[N] {};
        consteval Name(const char (&s)[N]) { std::ranges::copy(s, s + N, data); }
    };
    template <std::size_t N> Name(const char (&)[N]) -> Name<N>;
    template <std::size_t N> consteval Name<N> json_name(
        const char(&value)[N] ) {
        return {value};
    }
    template <class T>
    struct is_name : std::false_type {};

    template <std::size_t N>
    struct is_name<Name<N>> : std::true_type {};

    template <class T>
    inline constexpr bool is_name_v =
        is_name<std::remove_cvref_t<T>>::value;

    template <typename T>
    concept JsonArray = std::ranges::input_range<T>
        && !std::convertible_to<T const&, std::string_view>;

    template <typename T>
    concept JsonMap = JsonArray<T>
        && requires { typename T::key_type; typename T::mapped_type; };

    template<typename T> void toJson(T const&v, Json::Value& target);
    template <JsonArray T> requires (!JsonMap<T>)
    void toJson(T const& v, Json::Value& target);
    template <JsonArray T> requires (!JsonMap<T>)
    void fromJson(T& v, Json::Value const& src);
    template <JsonMap T>
    void toJson(T const& v, Json::Value& target);
    template <JsonMap T>
    void fromJson(T & v, Json::Value const& src);

    template<typename T>
    void toJson(T const&v, Json::Value &target)
    {
        constexpr auto ctx = std::meta::access_context::unprivileged();
        constexpr static auto data_members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx));
        template for (constexpr auto member : data_members) {
            using MemberType = [:std::meta::type_of(member):];

            string_view default_name = std::meta::identifier_of(member);
            const char* override_name = nullptr;
            constexpr static auto annotations = std::define_static_array(std::meta::annotations_of(member));
            //constexpr bool exclude = !std::meta::annotations_of_with_type(
            //    ^^T, ^^ExcludeTag).empty();
            //if constexpr (exclude)
            //    continue;
            bool skip = false;
            template for (constexpr auto a: annotations)
            {
                constexpr auto r_annotation_type = std::meta::type_of(a);
                constexpr auto value = std::meta::extract<typename[: r_annotation_type :]>(a);
                if constexpr (is_name_v<typename [:r_annotation_type:]>)
                    override_name = value.data;
                if constexpr (std::meta::constant_of(a) == std::meta::reflect_constant(Exclude))
                    skip = true;
            }
            if (skip)
                continue;
            Json::Value mt;
            toJson(v.[:member:], mt);
            target[override_name ? override_name : default_name.data()] = mt;
        }
    }
    template<typename T>
    void fromJson(T& target, Json::Value const& value)
    {
        constexpr auto ctx = std::meta::access_context::unprivileged();
        constexpr static auto data_members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx));
        template for (constexpr auto member : data_members) {
            using MemberType = [:std::meta::type_of(member):];

            string_view default_name = std::meta::identifier_of(member);
            const char* override_name = nullptr;
            constexpr static auto annotations = std::define_static_array(std::meta::annotations_of(member));
            bool skip = false;
            bool required = false;
            template for (constexpr auto a: annotations)
            {
                constexpr auto r_annotation_type = std::meta::type_of(a);
                constexpr auto value = std::meta::extract<typename[: r_annotation_type :]>(a);
                if constexpr (is_name_v<typename [:r_annotation_type:]>)
                    override_name = value.data;
                if constexpr (std::meta::constant_of(a) == std::meta::reflect_constant(Exclude))
                    skip = true;
                if constexpr (std::meta::constant_of(a) == std::meta::reflect_constant(Required))
                    required = true;
            }
            if (skip)
                continue;
            auto const&v = value[override_name ? override_name : default_name.data()];
            if (!v.isNull())
                fromJson(target.[:member:], v);
            else if (required)
                throw std::runtime_error(std::string("Required field ") + default_name + " missing in JSON object");
        }
    }
#define JS_PLAIN(T) \
    template<> \
    inline void toJson(T const& v, Json::Value &target) \
    { \
        target = v; \
    } \
    template<> inline void fromJson(T& v, Json::Value const& src) { v = src.as<T>();}

JS_PLAIN(std::string)
JS_PLAIN(uint32_t)
JS_PLAIN(int32_t)
JS_PLAIN(uint64_t)
JS_PLAIN(int64_t)
JS_PLAIN(float)
JS_PLAIN(double)
#undef JS_PLAIN

    template<typename T>
    void toJsonPtr(T const&v, Json::Value &target)
    {
        if (!v)
            return;
        toJson(*v, target);
    }
    template<typename T>
    void fromJsonPtr(T&v, Json::Value const& src)
    {
        if (src.isNull())
            return;
        v.emplace();
        fromJson(*v, src);
    }
#define JS_PTR(T) \
    template<typename P> void toJson(T<P> const&v, Json::Value & target) { toJsonPtr(v, target);}\
    template<typename P> void fromJson(T<P> & v, Json::Value const& src) {fromJsonPtr(v, src);}

    JS_PTR(std::unique_ptr)
    JS_PTR(std::shared_ptr)
    JS_PTR(std::optional)
#undef JS_PTR
    template <JsonArray T> requires (!JsonMap<T>)
    void toJson(T const& v, Json::Value& target)
    {
        target = Json::Value(Json::arrayValue);
        for (auto const& e : v) {
            Json::Value ev;
            toJson(e, ev);
            target.append(ev);
        }
    }

    template <JsonArray T> requires (!JsonMap<T>)
    void fromJson(T& v, Json::Value const& src)
    {
        for (auto const& e : src) {
            typename T::value_type vv;
            fromJson(vv, e);
            v.push_back(vv);
        }
    }

    template <JsonMap T>
    void toJson(T const& v, Json::Value& target)
    {
        target = Json::Value(Json::objectValue);
        for (auto const& [k, e] : v) {
            Json::Value ev;
            toJson(e, ev);
            target[k] = ev;
        }
    }

    template <JsonMap T>
    void fromJson(T & v, Json::Value const& src)
    {
        for (auto const& n: src.getMemberNames()) {
            typename T::key_type kk;
            typename T::mapped_type vv;
            fromJson(kk, n);
            fromJson(vv, src[n]);
            v[kk] = vv;
        }
    }
    class JsonSerializable {
    public:
        JsonSerializable() = default;
        ~JsonSerializable() = default;
    public:
        string toJson(this auto&& self) {
           Json::Value target;
           ::jsonser::toJson(self, target);
           std::stringstream ss;
           ss << target;
           return ss.str();
        }
        void fromJson(this auto&& self, std::string const& j)
        {
            std::stringstream ss(j);
            Json::Value jv;
            ss >> jv;
            ::jsonser::fromJson(self, jv);
        }
    };
}

#ifdef TEST_JSONSER
struct User : public jsonser::JsonSerializable {
    // A username field, which is public information
    string username;

    // A password field, which is sensitive (private) information
    [[=jsonser::Exclude]]
    string password;

    std::vector<std::string> roles;

    // A user ID field, which is public information
    [[=jsonser::json_name("user_id")]]
    uint32_t id;
};

int main() {
    User bob {
        .username = "Bob123",
        .password = "a0Q4}B*9^7&£",
        .roles = {"admin", "user"},
        .id = 123456789,
    };

    // prints bob = {"username": "Bob123", "id": 123456789}
    std::cerr << "bob = " << bob.toJson() << std::endl;
    std::cerr << bob.roles.size() << std::endl;
    User bbob;
    bbob.fromJson(bob.toJson());
    std::cerr << "bbob = " << bbob.toJson() << std::endl;
}

#endif