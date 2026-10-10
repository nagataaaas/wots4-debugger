#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    auto loader=LoadLibraryA(argv[1]);
    auto create=loader?(HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN))
        GetProcAddress(loader,"DirectInput8Create"):nullptr;
    IDirectInput8W* input=nullptr;
    HRESULT result=create?create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,
        (void**)&input,nullptr):E_FAIL;
    printf("DirectInput forwarding result=%08lx created=%d\n",result,input!=nullptr);
    if(input) input->Release();
    auto diagnostic=GetModuleHandleW(L"mod.dll");
    printf("diagnostic loaded=%d\n",diagnostic!=nullptr);
    __try { RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr); }
    __except(EXCEPTION_EXECUTE_HANDLER) { puts("exception reaches application handler"); }
    return SUCCEEDED(result)&&diagnostic?0:1;
}
