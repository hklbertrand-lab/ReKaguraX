// Dumps the decrypted/decompressed XEX image and its sections. Usage: xex_dump <in.xex> <out.bin>
#include <cstdio>
#include <vector>
#include <image.h>

int main(int argc, char** argv)
{
    FILE* f = fopen(argv[1], "rb");
    std::vector<uint8_t> buf;
    fseek(f, 0, SEEK_END); buf.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    fread(buf.data(), 1, buf.size(), f); fclose(f);

    Image img = Image::ParseImage(buf.data(), buf.size());
    printf("base %zx size %x entry %zx\n", img.base, img.size, img.entry_point);
    for (auto& s : img.sections)
        printf("%-8s %zx %x flags %x\n", s.name.c_str(), s.base, s.size, s.flags);

    FILE* o = fopen(argv[2], "wb");
    fwrite(img.data.get(), 1, img.size, o); fclose(o);
}
