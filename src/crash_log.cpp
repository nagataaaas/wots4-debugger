// クラッシュの記録: 例外（アクセス違反など）が起きた位置とレジスタを
// mods/debugger/crash_<日時>_<プロセス番号>.log に書き出す。
// ゲームのコードとセーブは書き換えず、例外も握りつぶさない。
#include <wots4/mod_api.h>
#include <cstdio>

static HANDLE output=INVALID_HANDLE_VALUE;
static uintptr_t gameBase;
static volatile LONG busy, count, initialized;
static PVOID handler;

static LONG CALLBACK observe(EXCEPTION_POINTERS* info) {
    const auto e=info->ExceptionRecord;
    if(e->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION &&
        e->ExceptionCode!=EXCEPTION_ILLEGAL_INSTRUCTION &&
        e->ExceptionCode!=EXCEPTION_IN_PAGE_ERROR) return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedCompareExchange(&busy,1,0)) return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedIncrement(&count)<=32) {
        const auto c=info->ContextRecord;
        uintptr_t ip=(uintptr_t)e->ExceptionAddress;
        MEMORY_BASIC_INFORMATION region={};
        wchar_t module[MAX_PATH]=L"<private/unmapped>";
        uintptr_t owner=0;
        if(VirtualQuery((void*)ip,&region,sizeof(region))) {
            owner=(uintptr_t)region.AllocationBase;
            GetModuleFileNameW((HMODULE)owner,module,MAX_PATH);
        }
        const char* candidate="other";
        uintptr_t preferred=ip-gameBase+0x400000;
        if(preferred==0x54a883||preferred==0x54a848) candidate="ending weapon: missing actor";
        else if(preferred==0x6d4202) candidate="ending weapon: missing model";
        else if(preferred==0x406acd||preferred==0x406b2a) candidate="Steam stats: missing interface";
        char line[1200];
        int length=sprintf_s(line,
            "first_chance=%ld code=%08lx ip=%08lx game_preferred_va=%08lx "
            "module=%ls module_rva=%08lx operation=%lu target=%08lx candidate=%s "
            "eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx\r\n",
            count,e->ExceptionCode,(unsigned long)ip,(unsigned long)preferred,module,
            (unsigned long)(ip-owner),e->NumberParameters>0?(unsigned long)e->ExceptionInformation[0]:0,
            e->NumberParameters>1?(unsigned long)e->ExceptionInformation[1]:0,candidate,
            c->Eax,c->Ecx,c->Edx,c->Esi,c->Edi,c->Esp);
        if(length>0) {
            DWORD written;WriteFile(output,line,(DWORD)length,&written,nullptr);
            FlushFileBuffers(output);
        }
    }
    InterlockedExchange(&busy,0);
    // Never swallow the exception, change registers, or replace other handlers.
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" BOOL WINAPI Wots4ModInitialize(const wots4::ModContext* context) {
    if(!context||context->size<sizeof(*context)||context->version!=wots4::modApiVersion||
        !context->directory||!context->module||!context->game) return FALSE;
    if(InterlockedCompareExchange(&initialized,1,0)) return handler!=nullptr;
    wchar_t path[MAX_PATH]; SYSTEMTIME now;GetLocalTime(&now);
    if(swprintf_s(path,L"%scrash_%04u%02u%02u_%02u%02u%02u_%03u_%lu.log",
        context->directory,now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,
        now.wMilliseconds,GetCurrentProcessId())<0) return FALSE;
    output=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE) return FALSE;
    gameBase=(uintptr_t)context->game;
    char line[240];int length=sprintf_s(line,
        "diagnostics_only=1 game_base=%08lx pid=%lu first_chance_records_are_not_proof_of_fatal_crash=1\r\n",
        (unsigned long)gameBase,GetCurrentProcessId());
    DWORD written;WriteFile(output,line,(DWORD)length,&written,nullptr);FlushFileBuffers(output);
    HMODULE pinned;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(observe),&pinned)) {CloseHandle(output);output=INVALID_HANDLE_VALUE;return FALSE;}
    handler=AddVectoredExceptionHandler(0,observe);
    if(!handler) {CloseHandle(output);output=INVALID_HANDLE_VALUE;return FALSE;}
    return TRUE;
}
BOOL WINAPI DllMain(HMODULE,DWORD,void*) { return TRUE; }
