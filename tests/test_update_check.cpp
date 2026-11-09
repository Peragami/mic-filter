// test_update_check.cpp -- host tests for client/src/update_check.h.
#include <cstdio>
#include <string>

#include "../client/src/update_check.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    const std::string manifest =
        "{\n"
        "  \"version\": \"0.10.0\",\n"
        "  \"url\": \"https://raw.githubusercontent.com/Peragami/mic-filter/main/dist/update/MicFilterClient-v0.10.0.zip\",\n"
        "  \"sha256\": \"0123456789abcdef\",\n"
        "  \"notes\": \"passive calibration\"\n"
        "}\n";
    const UpdateInfo info = updatecheck::parseManifest (manifest);
    CHECK (info.ok);
    CHECK (info.version == "0.10.0");
    CHECK (info.url.find ("MicFilterClient-v0.10.0.zip") != std::string::npos);
    CHECK (info.sha256 == "0123456789abcdef");
    CHECK (info.notes == "passive calibration");

    // Missing required field -> not ok.
    CHECK (!updatecheck::parseManifest ("{\"version\": \"1.0.0\"}").ok);
    CHECK (!updatecheck::parseManifest ("not json").ok);
    CHECK (!updatecheck::parseManifest ("").ok);

    // Version ordering, incl. two-digit components and v-prefix.
    CHECK (updatecheck::compareVersions ("0.10.0", "0.9.0") > 0);
    CHECK (updatecheck::compareVersions ("0.9.0", "0.10.0") < 0);
    CHECK (updatecheck::compareVersions ("v1.0.0", "0.99.9") > 0);
    CHECK (updatecheck::compareVersions ("0.9.0", "0.9.0") == 0);
    CHECK (updatecheck::compareVersions ("0.9", "0.9.0") == 0);
    CHECK (updatecheck::compareVersions ("0.9.1", "0.9.0") > 0);

    // isNewer: garbage remote never triggers an update.
    CHECK (updatecheck::isNewer ("0.10.0", "0.9.0"));
    CHECK (!updatecheck::isNewer ("0.9.0", "0.9.0"));
    CHECK (!updatecheck::isNewer ("0.8.0", "0.9.0"));
    CHECK (!updatecheck::isNewer ("garbage", "0.9.0"));
    CHECK (!updatecheck::isNewer ("", "0.9.0"));

    if (failures == 0) std::printf ("test_update_check: ALL PASS\n");
    else std::printf ("test_update_check: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
