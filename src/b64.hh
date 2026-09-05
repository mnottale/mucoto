#pragma once
#include <string>
#include <vector>

namespace b64
{
    inline const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    inline std::string encode(void const* data, size_t size)
    {
        auto src = (unsigned char const*)data;
        std::string res;
        res.reserve((size + 2) / 3 * 4);
        for (size_t i = 0; i < size; i += 3)
        {
            unsigned v = src[i] << 16;
            if (i + 1 < size)
                v |= src[i + 1] << 8;
            if (i + 2 < size)
                v |= src[i + 2];
            res += alphabet[(v >> 18) & 63];
            res += alphabet[(v >> 12) & 63];
            res += i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
            res += i + 2 < size ? alphabet[v & 63] : '=';
        }
        return res;
    }

    inline std::vector<unsigned char> decode(std::string const& src)
    {
        static int rev[256];
        static bool init = false;
        if (!init)
        {
            for (int i = 0; i < 256; i++)
                rev[i] = -1;
            for (int i = 0; i < 64; i++)
                rev[(unsigned char)alphabet[i]] = i;
            init = true;
        }
        std::vector<unsigned char> res;
        res.reserve(src.size() / 4 * 3);
        unsigned v = 0;
        int bits = 0;
        for (char c: src)
        {
            int d = rev[(unsigned char)c];
            if (d < 0)
                continue;
            v = (v << 6) | d;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                res.push_back((v >> bits) & 0xFF);
            }
        }
        return res;
    }
}
