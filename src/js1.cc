#include <type_traits>
#include <string_view>
#include <iostream>
#include <meta>
#include <json/value.h>
#include <json/writer.h>

using std::meta::access_context;
using std::convertible_to;
using std::remove_cvref_t;
using std::string;
using std::string_view;

namespace jsonser
{
    // Define a marker annotation named Exclude,
    // with a unique, anonymous, empty struct
    inline constexpr struct {} Exclude;
    inline constexpr struct {} Required;
    struct Name
    {
        const char* name;
    };
    consteval Name json_name(const char* value) {
        return {value};
    }
    template<typename T>
    void toJson(T const&v, Json::Value &target)
    {
        constexpr auto ctx = std::meta::access_context::unprivileged();
        constexpr static auto data_members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx));
        template for (constexpr auto member : data_members) {
            using MemberType = [:std::meta::type_of(member):];

            // Additional check for the Exclude annotation
            //if constexpr (std::meta::has_annotation(member, Exclude)) {
            //    continue;
            //}
            string_view default_name = std::meta::identifier_of(member);
            const char* override_name = nullptr;
            constexpr static auto annotations = std::define_static_array(std::meta::annotations_of_with_type(member, ^^Name));
            template for (constexpr auto a: annotations)
            {
                constexpr auto r_annotation_type = std::meta::type_of(a);
                constexpr auto value = std::meta::extract<typename[: r_annotation_type :]>(a);
                //constexpr auto value = std::meta::extract<Name>(a);
                override_name = value.name;
            }
            Json::Value mt;
            toJson(v.[:member:], mt);
            target[override_name ? override_name : default_name.data()] = mt;
        }
    }
    template<>
    void toJson(unsigned int const& v, Json::Value &target)
    {
        target = v;
    }
        template<>
    void toJson(std::string const& v, Json::Value &target)
    {
        target = v.c_str();
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
    };
}
struct User : public jsonser::JsonSerializable {
    // A username field, which is public information
    string username;

    // A password field, which is sensitive (private) information
    [[=jsonser::Exclude]]
    string password;

    // A user ID field, which is public information
    [[=jsonser::json_name("user_id")]]
    uint32_t id;
};

int main() {
    User bob {
        .username = "Bob123",
        .password = "a0Q4}B*9^7&£",
        .id = 123456789,
    };

    // prints bob = {"username": "Bob123", "id": 123456789}
    std::cerr << "bob = " << bob.toJson() << std::endl;
}