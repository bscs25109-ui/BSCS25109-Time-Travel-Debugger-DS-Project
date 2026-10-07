#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdio>
#include <cstdint>
using namespace std;

int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t startPos = ftell(f);              
    int32_t size = (int32_t)text.size();

    fwrite(&offsetField, sizeof(int64_t), 1, f);   
    fwrite(&size, sizeof(int32_t), 1, f);          
    fwrite(text.data(), 1, size, f);               

    return startPos;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetField;
    int32_t size;

    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
    {
        return -1;
    }

    outText.resize(size);

    if (size > 0 && fread(&outText[0], 1, size, f) != (size_t)size)
    {
        return -1;
    }

    return offsetField;
}
int main()
{
    FILE* f = fopen("test.bin", "wb");
    if (!f) { cerr << "cannot open\n"; return 1; }

    string lines[] = { "func foo b", "set a 10", "add b a", "func_end" };
    for (const string& line : lines)
    {
        int64_t pos = ftell(f);                    // normal line: offset field = own position
        writeResolveRecord(f, pos, line);
        cout << "wrote at " << pos << ": " << line << endl;
    }
    fclose(f);

    f = fopen("test.bin", "rb");
    string text;
    int64_t off;
    while ((off = readResolveRecord(f, text)) != -1)
        cout << "read offset=" << off << " text=" << text << endl;
    fclose(f);

    return 0;
}