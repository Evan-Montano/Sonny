// jsonutil_tests.cpp
// Tests for the Json Utility header file.

#include "common/jsonutil.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Constructor tests

TEST(JsonUtilityConstructorTests, ConstructsFromValidJsonString)
{
    const std::string jsonString = R"({"name":"Sonny","age":3})";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.Get<std::string>("name"), "Sonny");
    EXPECT_EQ(json.Get<int>("age"), 3);
}

// FromJson tests

TEST(JsonUtilityFromJsonTests, ConstructsFromJsonObject)
{
    const nlohmann::json source = {
        {"name", "Sonny"},
        {"age", 3},
        {"active", true}
    };

    const Common::JsonUtility json =
        Common::JsonUtility::FromJson(source);

    EXPECT_EQ(json.Get<std::string>("name"), "Sonny");
    EXPECT_EQ(json.Get<int>("age"), 3);
    EXPECT_TRUE(json.Get<bool>("active"));
}

TEST(JsonUtilityFromJsonTests, ConstructsFromJsonArray)
{
    const nlohmann::json source = {
        "Sonny",
        "Fritz"
    };

    const Common::JsonUtility json =
        Common::JsonUtility::FromJson(source);

    EXPECT_EQ(json.At(0).Get<std::string>(), "Sonny");
    EXPECT_EQ(json.At(1).Get<std::string>(), "Fritz");
}

// FromFile tests

TEST(JsonUtilityFromFileTests, ConstructsFromValidJsonFile)
{
    const std::filesystem::path filePath =
        std::filesystem::temp_directory_path() / "jsonutil_test.json";

    {
        std::ofstream file(filePath);
        ASSERT_TRUE(file.is_open());

        file << R"({"name":"Sonny","age":3,"active":true})";
    }

    const Common::JsonUtility json =
        Common::JsonUtility::FromFile(filePath);

    EXPECT_EQ(json.Get<std::string>("name"), "Sonny");
    EXPECT_EQ(json.Get<int>("age"), 3);
    EXPECT_TRUE(json.Get<bool>("active"));

    std::filesystem::remove(filePath);
}

TEST(JsonUtilityFromFileTests, ThrowsForMissingJsonFile)
{
    const std::filesystem::path filePath =
        std::filesystem::temp_directory_path() / "jsonutil_missing.json";

    std::filesystem::remove(filePath);

    EXPECT_THROW(
        Common::JsonUtility::FromFile(filePath),
        std::runtime_error
    );
}

TEST(JsonUtilityFromFileTests, ThrowsForInvalidJsonFile)
{
    const std::filesystem::path filePath =
        std::filesystem::temp_directory_path() / "jsonutil_invalid.json";

    {
        std::ofstream file(filePath);
        ASSERT_TRUE(file.is_open());

        file << R"({"name":"Sonny",})";
    }

    EXPECT_THROW(
        Common::JsonUtility::FromFile(filePath),
        std::invalid_argument
    );

    std::filesystem::remove(filePath);
}

// Get tests

TEST(JsonUtilityGetTests, GetsStringValue)
{
    const std::string jsonString = R"({"name":"Sonny"})";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.Get<std::string>("name"), "Sonny");
}

TEST(JsonUtilityGetTests, GetsIntegerValue)
{
    const std::string jsonString = R"({"age":3})";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.Get<int>("age"), 3);
}

TEST(JsonUtilityGetTests, GetsFloatingPointValue)
{
    const std::string jsonString = R"({"price":12.5})";
    const Common::JsonUtility json(jsonString);

    EXPECT_DOUBLE_EQ(json.Get<double>("price"), 12.5);
}

TEST(JsonUtilityGetTests, GetsBooleanValue)
{
    const std::string jsonString = R"({"active":true})";
    const Common::JsonUtility json(jsonString);

    EXPECT_TRUE(json.Get<bool>("active"));
}

TEST(JsonUtilityGetTests, GetsArrayValue)
{
    const std::string jsonString = R"({"values":[1,2,3]})";
    const Common::JsonUtility json(jsonString);

    const std::vector<int> values =
        json.Get<std::vector<int>>("values");

    EXPECT_EQ(values, std::vector<int>({1, 2, 3}));
}

TEST(JsonUtilityGetTests, ThrowsForMissingKey)
{
    const std::string jsonString = R"({"name":"Sonny"})";
    const Common::JsonUtility json(jsonString);

    EXPECT_THROW(
        json.Get<std::string>("missing"),
        std::out_of_range
    );
}

TEST(JsonUtilityGetTests, ThrowsForIncorrectType)
{
    const std::string jsonString = R"({"age":3})";
    const Common::JsonUtility json(jsonString);

    EXPECT_THROW(
        json.Get<std::string>("age"),
        std::invalid_argument
    );
}

// Root Get tests

TEST(JsonUtilityRootGetTests, GetsRootString)
{
    const std::string jsonString = R"("Sonny")";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.Get<std::string>(), "Sonny");
}

TEST(JsonUtilityRootGetTests, GetsRootInteger)
{
    const std::string jsonString = "3";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.Get<int>(), 3);
}

TEST(JsonUtilityRootGetTests, GetsRootBoolean)
{
    const std::string jsonString = "true";
    const Common::JsonUtility json(jsonString);

    EXPECT_TRUE(json.Get<bool>());
}

TEST(JsonUtilityRootGetTests, GetsRootArray)
{
    const std::string jsonString = R"([1,2,3])";
    const Common::JsonUtility json(jsonString);

    const std::vector<int> values =
        json.Get<std::vector<int>>();

    EXPECT_EQ(values, std::vector<int>({1, 2, 3}));
}

TEST(JsonUtilityRootGetTests, ThrowsForIncorrectType)
{
    const std::string jsonString = R"("Sonny")";
    const Common::JsonUtility json(jsonString);

    EXPECT_THROW(
        json.Get<int>(),
        std::invalid_argument
    );
}

// TryGet tests

TEST(JsonUtilityTryGetTests, GetsExistingStringValue)
{
    const std::string jsonString = R"({"name":"Sonny"})";
    const Common::JsonUtility json(jsonString);

    std::string name;

    EXPECT_TRUE(json.TryGet("name", name));
    EXPECT_EQ(name, "Sonny");
}

TEST(JsonUtilityTryGetTests, GetsExistingIntegerValue)
{
    const std::string jsonString = R"({"age":3})";
    const Common::JsonUtility json(jsonString);

    int age = 0;

    EXPECT_TRUE(json.TryGet("age", age));
    EXPECT_EQ(age, 3);
}

TEST(JsonUtilityTryGetTests, ReturnsFalseForMissingKey)
{
    const std::string jsonString = R"({"name":"Sonny"})";
    const Common::JsonUtility json(jsonString);

    std::string name = "unchanged";

    EXPECT_FALSE(json.TryGet("missing", name));
    EXPECT_EQ(name, "unchanged");
}

TEST(JsonUtilityTryGetTests, ReturnsFalseForIncorrectType)
{
    const std::string jsonString = R"({"age":3})";
    const Common::JsonUtility json(jsonString);

    std::string age = "unchanged";

    EXPECT_FALSE(json.TryGet("age", age));
    EXPECT_EQ(age, "unchanged");
}

// At index tests

TEST(JsonUtilityAtIndexTests, GetsArrayStringValue)
{
    const std::string jsonString = R"(["Sonny","Fritz"])";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.At(0).Get<std::string>(), "Sonny");
    EXPECT_EQ(json.At(1).Get<std::string>(), "Fritz");
}

TEST(JsonUtilityAtIndexTests, GetsArrayIntegerValue)
{
    const std::string jsonString = R"([1,2,3])";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.At(0).Get<int>(), 1);
    EXPECT_EQ(json.At(1).Get<int>(), 2);
    EXPECT_EQ(json.At(2).Get<int>(), 3);
}

TEST(JsonUtilityAtIndexTests, GetsNestedObject)
{
    const std::string jsonString = R"([
        {"name":"Sonny"},
        {"name":"Fritz"}
    ])";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.At(0).Get<std::string>("name"), "Sonny");
    EXPECT_EQ(json.At(1).Get<std::string>("name"), "Fritz");
}

TEST(JsonUtilityAtIndexTests, ThrowsForNonArray)
{
    const std::string jsonString = R"({"name":"Sonny"})";
    const Common::JsonUtility json(jsonString);

    EXPECT_THROW(
        json.At(0),
        std::runtime_error
    );
}

TEST(JsonUtilityAtIndexTests, ThrowsForOutOfRangeIndex)
{
    const std::string jsonString = R"([1,2,3])";
    const Common::JsonUtility json(jsonString);

    EXPECT_THROW(
        json.At(3),
        std::out_of_range
    );
}

// At key tests

TEST(JsonUtilityAtKeyTests, GetsNestedObject)
{
    const std::string jsonString = R"({
        "user": {
            "name":"Sonny",
            "age":3
        }
    })";
    const Common::JsonUtility json(jsonString);

    const Common::JsonUtility user = json.At("user");

    EXPECT_EQ(user.Get<std::string>("name"), "Sonny");
    EXPECT_EQ(user.Get<int>("age"), 3);
}

TEST(JsonUtilityAtKeyTests, GetsNestedArray)
{
    const std::string jsonString = R"({
        "values":[1,2,3]
    })";
    const Common::JsonUtility json(jsonString);

    const Common::JsonUtility values = json.At("values");

    EXPECT_EQ(values.At(0).Get<int>(), 1);
    EXPECT_EQ(values.At(1).Get<int>(), 2);
    EXPECT_EQ(values.At(2).Get<int>(), 3);
}

TEST(JsonUtilityAtKeyTests, SupportsNestedAccess)
{
    const std::string jsonString = R"({
        "user": {
            "profile": {
                "name":"Sonny"
            }
        }
    })";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(
        json.At("user")
            .At("profile")
            .Get<std::string>("name"),
        "Sonny"
    );
}

TEST(JsonUtilityAtKeyTests, ThrowsForMissingKey)
{
    const std::string jsonString = R"({"name":"Sonny"})";
    const Common::JsonUtility json(jsonString);

    EXPECT_THROW(
        json.At("missing"),
        std::out_of_range
    );
}

// ToString tests

TEST(JsonUtilityToStringTests, ReturnsJsonString)
{
    const std::string jsonString = R"({"name":"Sonny","age":3})";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(
        json.ToString(),
        R"({"age":3,"name":"Sonny"})"
    );
}

TEST(JsonUtilityToStringTests, ReturnsArrayString)
{
    const std::string jsonString = R"([1,2,3])";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(json.ToString(), "[1,2,3]");
}

TEST(JsonUtilityToStringTests, ReturnsNestedJsonString)
{
    const std::string jsonString = R"({
        "user": {
            "name":"Sonny"
        }
    })";
    const Common::JsonUtility json(jsonString);

    EXPECT_EQ(
        json.At("user").ToString(),
        R"({"name":"Sonny"})"
    );
}