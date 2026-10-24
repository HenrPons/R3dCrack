#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <memory>
#include <deque>
#include <algorithm>
#include "../KeyFinder/RunControl.h"
#include "../KeyFinder/PuzzleCatalog.h"
#pragma comment(lib,"uxtheme.lib")
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"ole32.lib")

namespace {
const UINT LOG_MESSAGE=WM_APP+1, DONE_MESSAGE=WM_APP+2, HARDWARE_MESSAGE=WM_APP+3;
enum { PUZZLE=1001,GPU_ON,CPU_ON,GPU_LIMIT,CPU_LIMIT,START,PAUSE,STOP,TEST,RESULTS,COPY,DEVICE,BLOCKS,THREADS,POINTS };
const COLORREF BG=RGB(12,16,24), CARD=RGB(21,28,40), EDGE=RGB(42,55,75), TEXT=RGB(232,239,248), MUTED=RGB(149,166,190), ACCENT=RGB(255,91,105);
HWND win,puzzleBox,on[2],slider[2],pct[2],speed[2],status[2],combined,job,startBtn,pauseBtn,stopBtn,testBtn,logBox,wif,device,blocks,threads,points;
HBRUSH bgBrush,cardBrush; HFONT normalFont,smallFont,titleFont,bigFont;
bool paused=false,closing=false,detecting=true;
struct Engine { HANDLE process=NULL,reader=NULL; RunControl control; std::string result,last; bool test=false; int category=0,deviceId=0; };
std::deque<Engine> engines;
struct Gpu { int id; std::string name; };
std::vector<Gpu> gpus;
struct Hardware { std::vector<Gpu> gpus; std::string details; };
struct Reader { int index; HWND window; HANDLE pipe,process; };
struct Log { int index; std::string text; };
std::string text(HWND h) { std::string s(GetWindowTextLengthA(h)+1,'\0'); GetWindowTextA(h,&s[0],(int)s.size()); s.resize(strlen(s.c_str())); return s; }
void set(HWND h,const std::string &s) { SetWindowTextA(h,s.c_str()); }
std::string dir() { char p[MAX_PATH]={}; DWORD n=GetModuleFileNameA(NULL,p,MAX_PATH); if(!n || n>=MAX_PATH) return ""; std::string s(p); return s.substr(0,s.find_last_of("\\/")); }
std::string quote(const std::string &s) { return "\"" + s + "\""; }
bool running() { for(auto &e:engines) if(e.process) return true; return false; }
unsigned limit(int i) { return (unsigned)SendMessageA(slider[i],TBM_GETPOS,0,0); }
int puzzle() { auto i=SendMessageA(puzzleBox,CB_GETCURSEL,0,0); return i==CB_ERR ? 71 : (int)SendMessageA(puzzleBox,CB_GETITEMDATA,i,0); }
std::string rate(double n) { char b[64]; if(n>=1e6) snprintf(b,sizeof(b),"%.2f MKey/s",n/1e6); else if(n>=1e3) snprintf(b,sizeof(b),"%.2f KKey/s",n/1e3); else snprintf(b,sizeof(b),"%.0f Key/s",n); return b; }
void append(const std::string &s) {
    int len=GetWindowTextLengthA(logBox);
    if(len>50000) { SendMessageA(logBox,EM_SETSEL,0,15000); SendMessageA(logBox,EM_REPLACESEL,FALSE,(LPARAM)""); }
    len=GetWindowTextLengthA(logBox); SendMessageA(logBox,EM_SETSEL,len,len); SendMessageA(logBox,EM_REPLACESEL,FALSE,(LPARAM)s.c_str());
}
void controls() {
    bool active=running(); EnableWindow(startBtn,!active&&!detecting); EnableWindow(testBtn,!active&&!detecting); EnableWindow(pauseBtn,active); EnableWindow(stopBtn,active);
    for(HWND h:{puzzleBox,on[0],on[1],device,blocks,threads,points}) EnableWindow(h,!active);
    EnableWindow(on[0],!active&&!detecting&&!gpus.empty()); EnableWindow(device,!active&&!detecting&&!gpus.empty());
    set(pauseBtn,paused?"Resume":"Pause");
}
void updateLimits() {
    for(int i=0;i<2;++i) set(pct[i],std::to_string(limit(i))+"%");
    for(auto &e:engines) if(e.control.data) InterlockedExchange(&e.control.data->percent,paused?0:limit(e.category));
}
void saveSettings() {
    const auto path=dir()+"\\settings.ini";
    for(auto pair: {std::make_pair("Blocks",blocks),std::make_pair("Threads",threads),std::make_pair("Points",points)})
        WritePrivateProfileStringA("GPU",pair.first,text(pair.second).c_str(),path.c_str());
    WritePrivateProfileStringA("Load","GPU",std::to_string(limit(0)).c_str(),path.c_str());
    WritePrivateProfileStringA("Load","CPU",std::to_string(limit(1)).c_str(),path.c_str());
}
void stop() {
    for(auto &e:engines) if(e.process && e.control.data) InterlockedExchange(&e.control.data->stop,1);
    if(running()) set(job,"Saving checkpoints...");
    EnableWindow(stopBtn,FALSE); EnableWindow(pauseBtn,FALSE);
}
void loadResult(int i) {
    auto &e=engines[i]; std::ifstream f(e.result); std::string line,last;
    while(std::getline(f,line)) if(!line.empty()) last=line;
    if(last.empty() || last==e.last) return;
    e.last=last; std::istringstream in(last); std::string address,key,value; in>>address>>key>>value; if(!value.empty()) set(wif,value);
}
DWORD WINAPI readWorker(LPVOID pointer) {
    std::unique_ptr<Reader> r((Reader*)pointer); char b[4096]; DWORD count; std::string pending;
    while(ReadFile(r->pipe,b,sizeof(b),&count,NULL) && count) {
        pending.append(b,count); size_t end;
        while((end=pending.find('\n'))!=std::string::npos) {
            auto line=pending.substr(0,end); if(!line.empty() && line.back()=='\r') line.pop_back();
            auto m=new Log{r->index,line+"\r\n"}; if(!PostMessageA(r->window,LOG_MESSAGE,0,(LPARAM)m)) delete m;
            pending.erase(0,end+1);
        }
    }
    if(!pending.empty()) { auto m=new Log{r->index,pending+"\r\n"}; if(!PostMessageA(r->window,LOG_MESSAGE,0,(LPARAM)m)) delete m; }
    CloseHandle(r->pipe); WaitForSingleObject(r->process,INFINITE); DWORD code=1; GetExitCodeProcess(r->process,&code);
    PostMessageA(r->window,DONE_MESSAGE,r->index,code); return 0;
}
bool number(HWND h,bool zero=false) { auto s=text(h); return !s.empty() && s.size()<8 && s.find_first_not_of("0123456789")==std::string::npos && (zero || strtoul(s.c_str(),NULL,10)>0); }
bool startEngine(int i,bool test,const std::string &partition,unsigned slice,unsigned count) {
    auto &e=engines[i]; int category=e.category;
    GUID id; wchar_t wide[40]; char token[40];
    if(FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id,wide,40) ||
       !WideCharToMultiByte(CP_UTF8,0,wide,-1,token,40,NULL,NULL)) {
        append("Could not generate a private control channel.\r\n"); return false;
    }
    auto name=std::string("Local\\RedCrack-")+token;
    if(!e.control.create(name,limit(category))) { append("Could not create control channel.\r\n"); return false; }
    std::string kind=category?"cpu":"gpu";
    std::string base="p"+std::to_string(puzzle())+"_v3_"+kind+"_"+partition+
        "_slice"+std::to_string(slice)+"of"+std::to_string(count);
    e.result=dir()+"\\"+(test?"selftest_"+kind+std::to_string(e.deviceId)+"_"+std::to_string(GetTickCount64()):"results_"+base)+".txt"; e.last.clear(); e.test=test;
    std::string args=test?"--self-test-solved":"--puzzle "+std::to_string(puzzle())+" --partition "+partition+" --continue "+quote(dir()+"\\checkpoint_"+base+".txt");
    args+=" --compression compressed --follow --control "+quote(name)+" -o "+quote(e.result);
    if(category) args+=" --cpu";
    else args+=" -d "+std::to_string(e.deviceId)+" -b "+(test?"40":text(blocks))+" -t "+text(threads)+" -p "+(test?"64":text(points));
    if(!test) args += " --partition-index " + std::to_string(slice) + " --partition-count " + std::to_string(count);
    std::string worker=dir()+"\\clBitCrack.exe",command=quote(worker)+" "+args; std::vector<char> cmd(command.begin(),command.end()); cmd.push_back(0);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE input=NULL,output=NULL;
    if(!CreatePipe(&input,&output,&sa,0)) { e.control.close(); return false; }
    SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOA si={}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW; si.hStdOutput=si.hStdError=output; si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi={};
    BOOL ok=CreateProcessA(worker.c_str(),cmd.data(),NULL,NULL,TRUE,CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,NULL,dir().c_str(),&si,&pi);
    CloseHandle(output);
    if(!ok) { CloseHandle(input); e.control.close(); append("Could not open clBitCrack.exe. Keep the worker beside the GUI.\r\n"); return false; }
    CloseHandle(pi.hThread); e.process=pi.hProcess;
    auto r=new Reader{i,win,input,pi.hProcess}; e.reader=CreateThread(NULL,0,readWorker,r,0,NULL);
    if(!e.reader) { InterlockedExchange(&e.control.data->stop,1); CloseHandle(input); delete r; WaitForSingleObject(e.process,INFINITE); CloseHandle(e.process); e.process=NULL; e.control.close(); return false; }
    append("["+kind+"] "+(test?"Self-test started":"Search started with a dedicated range and checkpoint")+"\r\n"); return true;
}
void start(bool test) {
    if(running()||detecting) return;
    bool gpu=SendMessageA(on[0],BM_GETCHECK,0,0)==BST_CHECKED,cpu=SendMessageA(on[1],BM_GETCHECK,0,0)==BST_CHECKED;
    if(!gpu && !cpu) { MessageBoxA(win,"Enable GPU, CPU, or both.","Select engines",MB_OK); return; }
    if(gpu && (!number(blocks)||!number(threads)||!number(points))) { MessageBoxA(win,"Check the GPU parameters.","Invalid parameters",MB_OK); return; }
    saveSettings(); paused=false; set(wif,"");
    for(int c=0;c<2;++c) { set(speed[c],"0 Key/s"); set(status[c],"Idle"); }
    int selection=(int)SendMessageA(device,CB_GETCURSEL,0,0);
    const unsigned gpuCount=gpu?(selection==0?(unsigned)gpus.size():1U):0U;
    unsigned gpuSlice=0;
    append("Coverage: 100% across selected engines. Paused workers retain their assigned ranges.\r\n");
    for(int i=0;i<(int)engines.size();++i) {
        auto &e=engines[i];
        if((e.category && cpu) || (!e.category && gpu && (selection==0 || selection==e.deviceId+1))) {
            std::string partition=gpu&&cpu?(e.category?"cpu":"gpu"):"full";
            if(!startEngine(i,test,partition,e.category?0:gpuSlice++,e.category?1:gpuCount)) {
                append("Could not start every selected engine. Stopping the session.\r\n"); stop(); break;
            }
        }
    }
    set(job,running()?"Starting engines...":"Failed to start"); controls();
}
HWND make(const char *cls,const char *name,DWORD style,int id,int x,int y,int w,int h,HFONT font=NULL) {
    HWND c=CreateWindowExA(0,cls,name,WS_CHILD|WS_VISIBLE|style,x,y,w,h,win,(HMENU)(INT_PTR)id,NULL,NULL);
    SendMessageA(c,WM_SETFONT,(WPARAM)(font?font:normalFont),TRUE); return c;
}
HWND label(const char *s,int x,int y,int w,int h=22,HFONT font=NULL) { return make("STATIC",s,SS_LEFT,0,x,y,w,h,font); }
HWND edit(const char *s,int id,int x,int y,int w) { return make("EDIT",s,WS_TABSTOP|ES_AUTOHSCROLL|WS_BORDER,id,x,y,w,26); }
HWND button(const char *s,int id,int x,int y,int w) { return make("BUTTON",s,WS_TABSTOP|BS_OWNERDRAW,id,x,y,w,38); }
DWORD WINAPI detectHardware(LPVOID pointer) {
    HWND owner=(HWND)pointer; auto result=new Hardware;
    DWORD bytes=0; GetLogicalProcessorInformationEx(RelationProcessorPackage,nullptr,&bytes);
    std::vector<unsigned char> topology(bytes); unsigned sockets=0;
    if(bytes && GetLogicalProcessorInformationEx(RelationProcessorPackage,(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)topology.data(),&bytes)) {
        for(DWORD offset=0;offset<bytes;) {
            auto entry=(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(topology.data()+offset);
            if(!entry->Size) break; ++sockets; offset+=entry->Size;
        }
    }
    result->details="[Hardware] CPU: "+std::to_string(sockets)+" processor package(s); all processor groups enabled.\r\n";
    HKEY key; char model[256]={}; DWORD length=sizeof(model);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",0,KEY_READ,&key)==ERROR_SUCCESS) {
        if(RegQueryValueExA(key,"ProcessorNameString",NULL,NULL,(BYTE*)model,&length)==ERROR_SUCCESS) result->details += std::string("[Hardware] ")+model+"\r\n";
        RegCloseKey(key);
    }
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE input=NULL,output=NULL;
    std::string content;
    if(CreatePipe(&input,&output,&sa,0)) {
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);
        std::string exe=dir()+"\\clBitCrack.exe",command=quote(exe)+" --list-devices";
        std::vector<char> cmd(command.begin(),command.end()); cmd.push_back(0);
        STARTUPINFOA si={}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
        si.hStdOutput=si.hStdError=output; PROCESS_INFORMATION pi={};
        BOOL ok=CreateProcessA(exe.c_str(),cmd.data(),NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,dir().c_str(),&si,&pi);
        CloseHandle(output);
        if(ok) {
            CloseHandle(pi.hThread); ULONGLONG deadline=GetTickCount64()+20000;
            for(;;) {
                DWORD available=0,read=0; char buffer[4096];
                if(!PeekNamedPipe(input,NULL,0,NULL,&available,NULL)) break;
                if(available) {
                    if(!ReadFile(input,buffer,std::min(available,(DWORD)sizeof(buffer)),&read,NULL)) break;
                    content.append(buffer,read); continue;
                }
                if(WaitForSingleObject(pi.hProcess,20)==WAIT_OBJECT_0) {
                    // The process can flush stdout while we wait; drain it before exiting.
                    if(PeekNamedPipe(input,NULL,0,NULL,&available,NULL) && available) continue;
                    break;
                }
                if(GetTickCount64()>deadline) { TerminateProcess(pi.hProcess,1); WaitForSingleObject(pi.hProcess,2000); break; }
            }
            CloseHandle(pi.hProcess);
        }
        CloseHandle(input);
    }
    std::istringstream lines(content); std::string line; int id=-1;
    while(std::getline(lines,line)) {
        if(line.compare(0,3,"ID:")==0) id=atoi(line.c_str()+3);
        if(line.compare(0,5,"Name:")==0 && id>=0) {
            auto name=line.substr(5); auto begin=name.find_first_not_of(" \t"); if(begin!=std::string::npos) name.erase(0,begin);
            if(!name.empty()&&name.back()=='\r') name.pop_back();
            result->gpus.push_back({id,name}); result->details += "[Hardware] GPU "+std::to_string(id)+": "+name+"\r\n"; id=-1;
        }
    }
    if(result->gpus.empty()) result->details += "[Hardware] No OpenCL GPU detected. CPU search remains available. Check the GPU driver if a device is missing.\r\n";
    if(!PostMessageA(owner,HARDWARE_MESSAGE,0,(LPARAM)result)) delete result;
    return 0;
}
void createUi() {
    label("REDCRACK",24,20,430,36,titleFont); label("OPEN SOURCE   /   CPU + GPU",26,60,500,20,smallFont);
    combined=label("0 Key/s",750,20,285,40,bigFont); label("COMBINED SPEED",752,62,280,20,smallFont);
    label("Puzzle",24,102,60); puzzleBox=make("COMBOBOX","",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,PUZZLE,86,96,120,350);
    for(const auto &entry:PuzzleCatalog::all()) { int p=entry.bits; auto s="#"+std::to_string(p); auto idx=SendMessageA(puzzleBox,CB_ADDSTRING,0,(LPARAM)s.c_str()); SendMessageA(puzzleBox,CB_SETITEMDATA,idx,p); } SendMessageA(puzzleBox,CB_SETCURSEL,0,0);
    for(int i=0;i<2;++i) { on[i]=make("BUTTON",i?"Use CPU":"Use GPU",BS_AUTOCHECKBOX|WS_TABSTOP,i?CPU_ON:GPU_ON,240+i*150,98,120,26); SetWindowTheme(on[i],L"",L""); SendMessageA(on[i],BM_SETCHECK,BST_CHECKED,0); }
    job=label("Ready to start",650,102,380);
    for(int i=0;i<2;++i) {
        int x=i?566:48; label(i?"PROCESSOR / CPU":"GRAPHICS / GPU",x,164,300,22,smallFont);
        pct[i]=label(i?"50%":"80%",x+350,160,90,32,titleFont); speed[i]=label("0 Key/s",x,196,420,43,bigFont); status[i]=label("Idle",x,239,420,22,smallFont);
        slider[i]=make(TRACKBAR_CLASSA,"",WS_TABSTOP|TBS_HORZ|TBS_NOTICKS,i?CPU_LIMIT:GPU_LIMIT,x,269,440,28); SetWindowTheme(slider[i],L"",L"");
        SendMessageA(slider[i],TBM_SETRANGE,TRUE,MAKELONG(0,100)); SendMessageA(slider[i],TBM_SETPOS,TRUE,i?50:80);
        label("Active-time target  /  0% pauses this engine",x,302,440,20,smallFont);
    }
    device=make("COMBOBOX","",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,DEVICE,48,334,175,250);
    SendMessageA(device,CB_ADDSTRING,0,(LPARAM)"Detecting GPUs..."); SendMessageA(device,CB_SETCURSEL,0,0);
    label("B",233,338,16,20,smallFont); blocks=edit("120",BLOCKS,250,334,55);
    label("T",315,338,16,20,smallFont); threads=edit("256",THREADS,332,334,55);
    label("P",397,338,16,20,smallFont); points=edit("128",POINTS,414,334,68);
    startBtn=button("Start search",START,24,390,155); pauseBtn=button("Pause",PAUSE,191,390,110); stopBtn=button("Stop and save",STOP,313,390,145); testBtn=button("Self-test",TEST,470,390,125);
    button("Open results",RESULTS,607,390,220); button("Copy WIF",COPY,839,390,197);
    label("Found WIF",24,450,130); wif=edit("",0,160,446,876); SendMessageA(wif,EM_SETREADONLY,TRUE,0);
    label("ACTIVITY",24,491,200,20,smallFont); label("Independent CPU and GPU checkpoints / adjust load while searching",300,491,735,20,smallFont);
    logBox=make("EDIT","",WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,0,24,519,1012,190,smallFont); SendMessageA(logBox,EM_SETLIMITTEXT,100000,0);
    label("Percentages target active time, not temperature or power consumption.",24,726,1010,20,smallFont);
    label("Minimize to keep searching in the background. Stop saves progress before exiting.",24,750,1010,20,smallFont);
    append("Ready. Run the self-test before your first search on this computer.\r\n");
    append("v3 coverage: selected engines cover the full puzzle range. Earlier checkpoints are not imported automatically.\r\n");
    const auto config=dir()+"\\settings.ini";
    for(auto pair: {std::make_pair("Blocks",blocks),std::make_pair("Threads",threads),std::make_pair("Points",points)}) {
        char value[32]; GetPrivateProfileStringA("GPU",pair.first,text(pair.second).c_str(),value,sizeof(value),config.c_str());
        set(pair.second,value);
    }
    SendMessageA(slider[0],TBM_SETPOS,TRUE,std::min(100U,GetPrivateProfileIntA("Load","GPU",80,config.c_str())));
    SendMessageA(slider[1],TBM_SETPOS,TRUE,std::min(100U,GetPrivateProfileIntA("Load","CPU",50,config.c_str())));
    updateLimits(); controls(); SetTimer(win,1,250,NULL);
    HANDLE probe=CreateThread(NULL,0,detectHardware,win,0,NULL);
    if(probe) CloseHandle(probe); else { detecting=false; append("Hardware detection could not start. Restart the app.\r\n"); }
}
void tick() {
    double totals[2]={0,0}; unsigned active[2]={0,0},initializing[2]={0,0}; bool stopping=false;
    for(int i=0;i<(int)engines.size();++i) {
        auto &e=engines[i]; if(!e.process||!e.control.data) continue;
        int c=e.category; ++active[c]; LONG st=InterlockedCompareExchange(&e.control.data->state,0,0);
        if(st==0) ++initializing[c];
        if(!paused&&limit(c)>0) totals[c]+=(double)InterlockedCompareExchange64(&e.control.data->keysPerSecond,0,0);
        stopping=stopping||e.control.stopping();
        if(st==5) { loadResult(i); if(!e.test) stop(); }
    }
    for(int c=0;c<2;++c) if(active[c]) {
        set(speed[c],rate(totals[c]));
        set(status[c],initializing[c]?"Initializing hardware...":paused||limit(c)==0?"Paused":"Searching / automatic checkpoint");
    }
    set(combined,rate(totals[0]+totals[1]));
    if(running()&&!closing&&!stopping) set(job,paused?"Search paused":"Session active / live controls");
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    switch(msg) {
    case HARDWARE_MESSAGE: {
        std::unique_ptr<Hardware> hardware((Hardware*)l); gpus=hardware->gpus;
        detecting=false; SendMessageA(device,CB_RESETCONTENT,0,0);
        SendMessageA(device,CB_ADDSTRING,0,(LPARAM)(gpus.empty()?"No OpenCL GPUs":"All GPUs"));
        for(auto &gpu:gpus) {
            std::string name=std::to_string(gpu.id)+": "+gpu.name;
            SendMessageA(device,CB_ADDSTRING,0,(LPARAM)name.c_str());
            engines.emplace_back(); engines.back().deviceId=gpu.id;
        }
        engines.emplace_back(); engines.back().category=1;
        SendMessageA(device,CB_SETCURSEL,0,0);
        if(gpus.empty()) SendMessageA(on[0],BM_SETCHECK,BST_UNCHECKED,0);
        append(hardware->details); controls(); return 0;
    }
    case WM_CREATE: win=h; createUi(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; GetClientRect(h,&r); FillRect(dc,&r,bgBrush);
        auto ob=SelectObject(dc,cardBrush); HPEN p=CreatePen(PS_SOLID,1,EDGE); auto op=SelectObject(dc,p);
        RoundRect(dc,24,144,518,374,18,18); RoundRect(dc,542,144,1036,374,18,18);
        SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(p); EndPaint(h,&ps); return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: {
        HDC dc=(HDC)w; HWND c=(HWND)l; RECT r; GetWindowRect(c,&r); MapWindowPoints(NULL,h,(POINT*)&r,2);
        bool card=(r.top>=144&&r.top<374)||c==logBox||c==wif; SetTextColor(dc,TEXT); SetBkColor(dc,card?CARD:BG); return (LRESULT)(card?cardBrush:bgBrush);
    }
    case WM_DRAWITEM: {
        auto d=(DRAWITEMSTRUCT*)l; if(d->CtlType!=ODT_BUTTON) break;
        bool enabled=!(d->itemState&ODS_DISABLED),primary=d->CtlID==START;
        HBRUSH b=CreateSolidBrush(primary&&enabled?ACCENT:CARD); HPEN p=CreatePen(PS_SOLID,1,(d->itemState&ODS_FOCUS)?ACCENT:EDGE);
        auto ob=SelectObject(d->hDC,b),op=SelectObject(d->hDC,p); RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,10,10);
        SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,!enabled?MUTED:primary?BG:TEXT); SelectObject(d->hDC,normalFont);
        RECT r=d->rcItem; if(d->itemState&ODS_SELECTED) OffsetRect(&r,1,1); auto s=text(d->hwndItem); DrawTextA(d->hDC,s.c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(d->hDC,ob); SelectObject(d->hDC,op); DeleteObject(b); DeleteObject(p); return TRUE;
    }
    case WM_NOTIFY: {
        auto n=(NMHDR*)l;
        if(n->code==NM_CUSTOMDRAW&&(n->hwndFrom==slider[0]||n->hwndFrom==slider[1])) {
            auto d=(NMCUSTOMDRAW*)l; if(d->dwDrawStage==CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if(d->dwDrawStage==CDDS_ITEMPREPAINT) { HBRUSH b=CreateSolidBrush(d->dwItemSpec==TBCD_THUMB?ACCENT:EDGE); FillRect(d->hdc,&d->rc,b); DeleteObject(b); return CDRF_SKIPDEFAULT; }
        } break;
    }
    case WM_HSCROLL: updateLimits(); return 0;
    case WM_TIMER: tick(); return 0;
    case WM_COMMAND:
        if(HIWORD(w) != BN_CLICKED) break;
        switch(LOWORD(w)) {
        case START: start(false); return 0;
        case TEST: start(true); return 0;
        case PAUSE: paused=!paused; updateLimits(); controls(); return 0;
        case STOP: stop(); return 0;
        case RESULTS: ShellExecuteA(h,"open",dir().c_str(),NULL,NULL,SW_SHOWNORMAL); return 0;
        case COPY: {
            auto value=text(wif); if(value.empty()) return 0;
            if(OpenClipboard(h)) {
                HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,value.size()+1);
                if(mem) { void *p=GlobalLock(mem); if(p) { memcpy(p,value.c_str(),value.size()+1); GlobalUnlock(mem); EmptyClipboard(); if(!SetClipboardData(CF_TEXT,mem)) GlobalFree(mem); } else GlobalFree(mem); }
                CloseClipboard();
            } return 0;
        }
        } break;
    case LOG_MESSAGE: {
        std::unique_ptr<Log> m((Log*)l); auto &e=engines[m->index]; append((e.category?std::string("[CPU] "):"[GPU "+std::to_string(e.deviceId)+"] ")+m->text); return 0;
    }
    case DONE_MESSAGE: {
        int i=(int)w; auto &e=engines[i]; loadResult(i); bool found=e.control.data&&e.control.data->state==5;
        append((e.category?std::string("[CPU] "):"[GPU "+std::to_string(e.deviceId)+"] ")+"Worker exited. Code "+std::to_string((DWORD)l)+"\r\n");
        WaitForSingleObject(e.reader,INFINITE); CloseHandle(e.reader); CloseHandle(e.process); e.reader=e.process=NULL; e.control.close(); set(speed[e.category],"0 Key/s");
        set(status[e.category],l?"Error / check the log":found?"Key confirmed":"Stopped / progress saved");
        if(l || (found&&!e.test)) stop();
        if(!running()) { paused=false; set(job,"Session ended"); set(combined,"0 Key/s"); }
        controls(); if(closing&&!running()) DestroyWindow(h); return 0;
    }
    case WM_CLOSE: saveSettings(); if(running()) { closing=true; stop(); } else DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h,msg,w,l);
}
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int show) {
    if(dir().empty()) { MessageBoxA(NULL,"Use a shorter installation path.","RedCrack",MB_OK|MB_ICONERROR); return 1; }
    SetProcessDPIAware(); INITCOMMONCONTROLSEX cc={sizeof(cc),ICC_BAR_CLASSES}; InitCommonControlsEx(&cc);
    bgBrush=CreateSolidBrush(BG); cardBrush=CreateSolidBrush(CARD);
    auto font=[](int size,int weight) { return CreateFontA(-size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI"); };
    normalFont=font(16,FW_NORMAL); smallFont=font(13,FW_NORMAL); titleFont=font(26,FW_SEMIBOLD); bigFont=font(32,FW_SEMIBOLD);
    WNDCLASSA wc={}; wc.lpfnWndProc=proc; wc.hInstance=instance; wc.lpszClassName="RedCrackHybrid"; wc.hIcon=LoadIconA(instance,MAKEINTRESOURCEA(101)); wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=bgBrush; RegisterClassA(&wc);
    RECT size={0,0,1060,786}; DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN; AdjustWindowRect(&size,style,FALSE);
    HWND h=CreateWindowExA(0,wc.lpszClassName,"RedCrack | CPU + GPU",style,CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,NULL,NULL,instance,NULL);
    if(!h) return 1; BOOL dark=TRUE; DwmSetWindowAttribute(h,20,&dark,sizeof(dark)); ShowWindow(h,show); UpdateWindow(h);
    MSG msg; while(GetMessageA(&msg,NULL,0,0)>0) if(!IsDialogMessageA(h,&msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    DeleteObject(normalFont); DeleteObject(smallFont); DeleteObject(titleFont); DeleteObject(bigFont); DeleteObject(bgBrush); DeleteObject(cardBrush); return (int)msg.wParam;
}
