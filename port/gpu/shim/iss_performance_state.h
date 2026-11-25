#pragma once
#include <cstdio>
#include <cstring>
namespace IssPerformance {
struct State { int fps=60; bool vsync=true; bool auto_quality=false; };
inline bool Read(const char* path, State& state) {
    if(!path || !*path)return false;
    FILE* f=std::fopen(path,"r");if(!f)return false;
    int fps=-1,vsync=-1,quality=0,complete=0;char line[128];
    while(std::fgets(line,sizeof(line),f)) {
        int value;
        if(std::sscanf(line,"fps_limit=%d",&value)==1)fps=value;
        else if(std::sscanf(line,"vsync=%d",&value)==1)vsync=value;
        else if(std::sscanf(line,"auto_quality=%d",&value)==1)quality=value;
        else if(std::sscanf(line,"complete=%d",&value)==1)complete=value;
    }
    const bool ok=!std::ferror(f);std::fclose(f);
    if(!ok || complete!=1 || (fps!=30&&fps!=60&&fps!=120&&fps!=240) || (vsync!=0&&vsync!=1))return false;
    if(quality!=0 && quality!=1)return false;
    state={fps,vsync!=0,quality!=0};return true;
}
}
