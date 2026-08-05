#include <doctest/doctest.h>

#include <core/steam/steam_token_blob.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

TEST_SUITE("steam_token_blob")
{
	TEST_CASE("SerializeAuthToken roundtrips username and token")
	{
		const auto blob = SerializeAuthToken("player", "steam_token_abc");
		std::string user;
		std::string token;
		REQUIRE(DeserializeAuthToken(blob.data(), blob.size(), user, token));
		CHECK(user == "player");
		CHECK(token == "steam_token_abc");
	}

	TEST_CASE("SerializeAuthToken roundtrips empty username")
	{
		const auto blob = SerializeAuthToken("", "tok");
		std::string user;
		std::string token;
		REQUIRE(DeserializeAuthToken(blob.data(), blob.size(), user, token));
		CHECK(user.empty());
		CHECK(token == "tok");
	}

	TEST_CASE("DeserializeAuthToken rejects short and overlong length fields")
	{
		std::string user;
		std::string token;
		CHECK_FALSE(DeserializeAuthToken(nullptr, 0, user, token));

		const char shortBuf[7] = {};
		CHECK_FALSE(DeserializeAuthToken(shortBuf, sizeof(shortBuf), user, token));

		// Claim username length 100 while only 4 payload bytes follow the length.
		std::vector<char> bad(8, 0);
		const uint32_t huge = 100;
		std::memcpy(bad.data(), &huge, 4);
		CHECK_FALSE(DeserializeAuthToken(bad.data(), bad.size(), user, token));
	}

	TEST_CASE("DeserializeAuthToken ignores trailing garbage after valid fields")
	{
		auto blob = SerializeAuthToken("u", "t");
		blob.push_back('X');
		blob.push_back('Y');

		std::string user;
		std::string token;
		REQUIRE(DeserializeAuthToken(blob.data(), blob.size(), user, token));
		CHECK(user == "u");
		CHECK(token == "t");
	}
}
