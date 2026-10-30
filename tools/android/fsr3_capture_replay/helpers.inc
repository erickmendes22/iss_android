#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sstream>
#include <cstdlib>
extern "C" void replaySetFrameIndex(FfxVkPortableUpscaleContext*,float);
void check(VkResult r){if(r!=VK_SUCCESS){std::fprintf(stderr,"vk=%d\n",int(r));std::abort();}}
std::vector<uint8_t> readFile(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);if(!in){std::fprintf(stderr,"missing %s\n",p.c_str());std::abort();}return {std::istreambuf_iterator<char>(in),{}};}
void writeFile(const std::filesystem::path& p,const std::vector<uint8_t>& b){std::ofstream out(p,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());}
std::map<std::string,std::string> readInfo(const std::filesystem::path& p){std::ifstream in(p);std::map<std::string,std::string> r;std::string line;while(std::getline(in,line)){auto x=line.find('=');if(x!=std::string::npos)r[line.substr(0,x)]=line.substr(x+1);}return r;}
std::array<float,2> pairValue(std::string s){auto i=s.find(',');return {std::stof(s.substr(0,i)),std::stof(s.substr(i+1))};}
