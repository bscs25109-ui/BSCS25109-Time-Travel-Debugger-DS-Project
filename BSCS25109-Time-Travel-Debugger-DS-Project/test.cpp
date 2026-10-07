// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

//#include <unistd.h>
//#include <sys/socket.h>
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <sstream> 
#include <stdexcept>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;          // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;      // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {
        Node* temp = new Node;
        temp->data = val;
        temp->next = top;
        top = temp;
        count++;

    }
    T pop()
    {
        if (isEmpty())
        {
            throw runtime_error("Stack is empty");
        }
        Node* temp = top;
        T ans = top->data;
        top = top->next;
        delete temp;
        count--;
        return ans;
    }
    T& peek()
    {
        if (isEmpty())
        {
            throw runtime_error("Stack is empty");
        }
        return top->data;
    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* temp = top;
        int32_t i = 0;
        while (temp != nullptr && i < maxLen)
        {
            out[i] = temp->data;
            temp = temp->next;
            i++;
        }
        return i;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:

    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        if (s == nullptr)
        {
            return;
        }
        TimelineNode* temp = new TimelineNode;
        temp->data = s;
        temp->next = nullptr;
        temp->prev = tail;

        if (head == nullptr)
        {
            head = temp;
            tail = temp;
        }
        else
        {
            tail->next = temp;
            tail = temp;
        }
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    while (getline(in, out))
    {
        if (!out.empty() && out.back() == '\r')
        {
            out.pop_back();
        }

        if (!out.empty())
        {
            return true;
        }
    }
    return false;
}
string firstWord(const string& line)
{
    stringstream word(line);
    string first;
    word >> first;
    return first;
}
string secondWord(const string& line)
{
    stringstream word(line);
    string first;
    string second;
    word >> first;
    word >> second;
    return second;
}
bool validateProgram(const char* sourcePath)
{
    ifstream fin(sourcePath, ios::binary);

    if (!fin.is_open())
    {
        cout << "error failed to open file " << endl;
        return false;
    }

    string line;
    bool temp = false;
    int countline = 0;
    while (readSourceLine(fin, line))
    {
        countline++;
        string first = firstWord(line);
        if (first == "func")
        {
            if (temp == true)
            {
                cout << "error at line " << countline << ": nested func not allowed" << endl;
                return false;
            }
            temp = true;
        }
        else if (first == "func_end")
        {
            if (temp == false)
            {
                cout << "error at line " << countline << ": func_end without matching func" << endl;
                return false;
            }
            temp = false;
        }
    }

    if (temp == true)
    {
        cout << "error at line " << countline << ": func without matching func_end" << endl;
        return false;
    }

    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t pos = ftell(f);
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    int32_t size = text.size();
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, size, f);
    return pos;


}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int32_t size;
    int64_t offsetField;
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
    {
        return -1;
    }
    outText.resize(size);

    if (fread(&outText[0], 1, size, f) != size)
    {
        return -1;
    }
    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 

    ifstream fin(sourcePath, ios::binary);
    if (!fin.is_open())
    {
        cout << "error failed to open file " << endl;
        return -1;
    }


    FILE* fout = fopen(resolveBinPath, "wb");
    if (!fout)
    {
        cout << "error failed to open resolve file " << endl;
        return -1;
    }

    string line;
    while (readSourceLine(fin, line))
    {
        int64_t pos = ftell(fout);
        string word1 = firstWord(line);
        string word2 = secondWord(line);

        if (word1 == "func")
        {

            funcArray[funcCount].funcName = word2;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
        }
        else if (word1 == "call")
        {
            patches[patchCount].byteOffsetOfOffsetField = pos;
            patches[patchCount].targetFuncName = word2;
            patchCount++;
        }
        writeResolveRecord(fout, pos, line);
    }

    fclose(fout);

    FILE* f = fopen(resolveBinPath, "r+b");
    if (f == nullptr)
    {
        cout << "error failed to open resolve file for patching" << endl;
        return -1;
    }


    for (int i = 0; i < patchCount; i++)
    {
        int64_t target = -1;
        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                target = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (target == -1)
        {
            cout << "error unresolved function call to " << patches[i].targetFuncName << endl;
            fclose(f);
            return -1;
        }
        fseek(f, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&target, sizeof(int64_t), 1, f);
    }
    fclose(f);

    for (int i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
        {
            return funcArray[i].byteOffsetInResolveBin;
        }
    }
    cout << "error main function not found" << endl;

    return -1;
}
// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated

    stringstream ss(line);
    string word;
    int32_t count = 0;
    while (ss >> word && count < maxTokens)
    {
        if (count == 0)
        {
            tokens[count].type = KEYWORD;
        }
        else if (count == 1)
        {
            tokens[count].type = IDENTIFIER;
        }
        else
        {
            tokens[count].type = PARAM;
        }
        tokens[count].text = word;
        count++;
    }
    return count;
}


Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}

Variable* findVar(Frame& frame, const string& name)
{
    // search in locals first
    for (int i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            return &frame.locals[i];
        }
    }
    // then search in argv
    for (int i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name)
        {
            return &frame.argv[i];
        }
    }
    return nullptr;
}

int32_t getVarValue(Frame& frame, const string& name)
{
    Variable* v = findVar(frame, name);
    if (v != nullptr)
    {
        return v->value;
    }
    return stoi(name);
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action

    FILE* f = fopen(resolveBinPath, "rb");
    if (f == nullptr)
    {
        cout << "error failed to open resolve file for execution" << endl;
        return;
    }

    Stack<Frame> callStack;


    string argSource[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);

    fseek(f, mainOffset, SEEK_SET);
    string line;

    readResolveRecord(f, line);

    while (true)
    {
        int64_t target = readResolveRecord(f, line);
        if (target == -1)
        {
            break;
        }

        Token tokens[MAX_TOKENS];
        int32_t count = tokenizeLine(line, tokens, MAX_TOKENS);
        string text = tokens[0].text;
        Frame& top = callStack.peek();


        if (text == "set")
        {
            Variable* v = findVar(top, tokens[1].text);
            if (v == nullptr)
            {
                v = &top.locals[top.localCount];
                v->name = tokens[1].text;
                top.localCount++;
            }
            v->value = getVarValue(top, tokens[2].text);
        }
        else if (text == "add" || text == "sub" || text == "mul" || text == "div")
        {
            Variable* v = findVar(top, tokens[1].text);
            int32_t val = getVarValue(top, tokens[2].text);

            if (text == "add")
            {
                v->value = v->value + val;
            }
            else if (text == "sub")
            {
                v->value = v->value - val;
            }
            else if (text == "mul")
            {
                v->value = v->value * val;
            }
            else if (text == "div")
            {
                if (val == 0)
                {
                    cout << "error division by zero in function " << endl;
                    break;;
                }
                v->value = v->value / val;
            }
        }
        else if (text == "call")
        {
            int32_t returnline = ftell(f);
            fseek(f, target, SEEK_SET);

            string funcline;
            readResolveRecord(f, funcline);
            Token functokens[MAX_TOKENS];
            int32_t funccount = tokenizeLine(funcline, functokens, MAX_TOKENS);
            int32_t argcount = count - 2;
            if (funccount - 2 != argcount)
            {
                cout << "error argument count mismatch in function call" << endl;
                break;
            }

            Frame nframe;
            nframe.func_name = functokens[1].text;
            nframe.argc = argcount;
            nframe.returnLine = returnline;
            nframe.localCount = 0;
            int32_t index = callStack.depth();

            for (int i = 0; i < argcount; i++)
            {
                string argvalue = tokens[i + 2].text;
                string argname = functokens[i + 2].text;
                nframe.argv[i].name = argname;
                nframe.argv[i].value = getVarValue(top, argvalue);
                argSource[index][i] = argvalue;
            }
            callStack.push(nframe);
        }
        else if (text == "func_end")
        {
            if (callStack.depth() == 1)
            {
                break;
            }
            if (callStack.isEmpty())
            {
                cout << "error func_end without matching func" << endl;
                break;
            }
            int32_t index = callStack.depth() - 1;
            Frame finalFrame = callStack.pop();
            Frame& callerFrame = callStack.peek();

            for (int i = 0; i < finalFrame.argc; i++)
            {
				string callerName = argSource[index][i]; 
                int32_t argvalue = finalFrame.argv[i].value;
                Variable* v = findVar(callerFrame, callerName);
                if (v != nullptr)
                {
                    v->value = argvalue;
                }
            }
            fseek(f, finalFrame.returnLine, SEEK_SET);
        }
        timeline.record(buildSnapshot(callStack));

    }
    fclose(f);

}

int32_t main()
{
    if (!validateProgram("source.bin")) return 1;
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset == -1) return 1;

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    cout << "steps = " << timeline.getStepCount() << endl;
    int step = 1;
    for (TimelineNode* n = timeline.begin(); n != nullptr; n = n->next)
    {
        Snapshot* s = n->data;
        cout << "Photo " << step++ << " (depth " << s->stackDepth << ")" << endl;
        for (int32_t d = 0; d < s->stackDepth; d++)
        {
            Frame& fr = s->callStack[d];
            cout << "   " << fr.func_name << ":";
            for (int32_t i = 0; i < fr.argc; i++)
                cout << " " << fr.argv[i].name << "=" << fr.argv[i].value;
            for (int32_t i = 0; i < fr.localCount; i++)
                cout << " " << fr.locals[i].name << "=" << fr.locals[i].value;
            cout << endl;
        }
    }
    return 0;
}