// The README's `### Code generator` example, as code that compiles and runs —
// and a check that the README still shows exactly this code.
//
// A hand-written example in a Markdown file stops matching the API the moment
// the API moves, and nothing notices until a reader copies it. Here the example
// is a real translation unit the compiler and this test binary see, and the
// mirror check below fails if the README's fenced block and this file ever
// disagree. Nothing asserts what the README *says* — only that the code in it is
// this code, and that this code works.

#include "sofab/sofab.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// --- 8< --- README `### Code generator` -------------------------------------

struct Point : sofab::OStreamMessage, sofab::IStreamMessage {
    static constexpr std::size_t _maxSize = 32;   // upper bound on the encoded size
    int32_t x = 0, y = 0;

    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl& os) const noexcept override {
        return os.write(1, x).write(2, y);
    }
    void deserialize(sofab::IStreamImpl& is, sofab::id id, size_t, size_t) noexcept override {
        switch (id) { case 1: is.read(x); break; case 2: is.read(y); break; }
    }
    std::vector<uint8_t> encode() const {
        sofab::OStreamInline<_maxSize> os; serialize(os);
        return {os.data(), os.data() + os.bytesUsed()};
    }
    static Point decode(const uint8_t* data, size_t len) {
        sofab::IStreamObject<Point> in{sofab::Limits{_maxSize}};   // this receiver's field-span budget
        in.feed(data, len); return *in;
    }
};

// --- >8 --- end of the mirrored block ---------------------------------------

static int failures = 0;

static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// The example round-trips, streams the same bytes through a window far smaller
// than the message, and decodes when fed one byte at a time.
static void theExampleRunsAndRoundTrips()
{
    Point pt; pt.x = 3; pt.y = 4;
    std::vector<uint8_t> wire = pt.encode();
    Point got = Point::decode(wire.data(), wire.size());
    check(got.x == 3 && got.y == 4, "one-shot round trip");

    std::vector<uint8_t> streamed;
    sofab::OStreamInline<1> os([&](std::span<const uint8_t> chunk) {
        streamed.insert(streamed.end(), chunk.begin(), chunk.end());
    });
    pt.serialize(os);
    os.flush();
    check(streamed == wire, "a one-byte window produces the one-shot bytes");

    sofab::IStreamObject<Point> in{sofab::Limits{Point::_maxSize}};
    auto r = in.feed(wire.data(), 1);                       // first byte…
    for (size_t i = 1; i < wire.size(); ++i)
        r = in.feed(wire.data() + i, 1);                    // …then the rest
    check(r.complete(), "byte-at-a-time feed reports complete");
    check((*in).x == 3 && (*in).y == 4, "byte-at-a-time decode");
}

// Every line of the README's C++ example must stand, in order, in this file.
static void theReadmeShowsTheCodeThisFileCompiles()
{
    std::ifstream f(SOFAB_README_PATH);
    if (!f) { std::fprintf(stderr, "FAIL: cannot open %s\n", SOFAB_README_PATH); ++failures; return; }
    std::stringstream ss; ss << f.rdbuf();
    const std::string doc = ss.str();

    const std::string heading = "### Code generator";
    size_t i = doc.find(heading);
    if (i == std::string::npos) { std::fprintf(stderr, "FAIL: no `%s` section\n", heading.c_str()); ++failures; return; }
    std::string rest = doc.substr(i);
    if (size_t e = rest.find("\n## ", heading.size()); e != std::string::npos) rest = rest.substr(0, e);
    size_t s = rest.find("```cpp");
    if (s == std::string::npos) { std::fprintf(stderr, "FAIL: section has no ```cpp example\n"); ++failures; return; }
    rest = rest.substr(s + 6);
    size_t e = rest.find("```");
    if (e == std::string::npos) { std::fprintf(stderr, "FAIL: unterminated code fence\n"); ++failures; return; }
    const std::string block = rest.substr(0, e);

    std::ifstream self(SOFAB_SELF_PATH);
    std::stringstream ms; ms << self.rdbuf();
    const std::string mirror = ms.str();

    size_t at = 0;
    std::istringstream lines(block);
    for (std::string line; std::getline(lines, line);) {
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b, line.find_last_not_of(" \t") - b + 1);
        // What follows the struct is statement code; it lives in the test above.
        if (line.rfind("Point pt;", 0) == 0) break;
        const size_t at2 = mirror.find(line, at);
        if (at2 == std::string::npos) {
            std::fprintf(stderr,
                "FAIL: README shows a line this file does not compile "
                "(or shows it out of order):\n\t%s\n", line.c_str());
            ++failures;
            continue;
        }
        at = at2 + line.size();
    }
}

int main()
{
    theExampleRunsAndRoundTrips();
    theReadmeShowsTheCodeThisFileCompiles();
    if (failures == 0) std::puts("test_readme_example: all checks passed");
    return failures == 0 ? 0 : 1;
}
