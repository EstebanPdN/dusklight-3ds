#include "JSystem/JUtility/JUTException.h"
#include "JSystem/JUtility/JUTConsole.h"
#include <dolphin/os.h>
#include <cstdarg>
#include <cstdio>
#include <cassert>
#include <string>
#include <stdexcept>
namespace {std::string logged;struct Panic:std::runtime_error{using std::runtime_error::runtime_error;};}
extern "C" void OSReport(const char* format,...){
    char text[256];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);logged+=text;
}
extern "C" void OSPanic(const char* file,int line,const char* format,...){
    assert(std::string(file)=="native-jut-test"&&line==31);
    // A formatted user string containing % must be passed as data, not format.
    assert(std::string(format)=="%s");
    char text[256];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);throw Panic(text);
}
int main(){
    JUTSetReportConsole(nullptr);JUTSetWarningConsole(nullptr);
    JUTReportConsole_f("native %s %u%%", "console", 100u);assert(logged=="native console 100%");logged.clear();
    JUTReportConsole("literal %s %n");assert(logged=="literal %s %n");logged.clear();
    JUTWarningConsole_f("warning %d%%",42);assert(logged=="warning 42%");logged.clear();
    JUTWarningConsole("literal warning %p");assert(logged=="literal warning %p");
    bool caught=false;try{JUTException::panic_f("native-jut-test",31,"fatal %u%% %s",100u,"%n");}catch(const Panic& e){caught=true;assert(std::string(e.what())=="fatal 100% %n");}assert(caught);
    std::string large(300,'x');caught=false;try{JUTException::panic_f("native-jut-test",31,"%s",large.c_str());}catch(const Panic& e){caught=true;assert(std::string(e.what()).size()==255);}assert(caught);
    puts("PASS: actual original JUT console/panic owners, Release logging, percent-as-data, bounded fatal text; platform test backend only");
}
