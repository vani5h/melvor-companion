#include "game_activity.hpp"
#include <windows.h>
#include <shlobj.h>
#include <brotli/decode.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <format>

namespace companion {
namespace {
// little-endian reader that throws instead of reading past the end
class Reader {
public:
    explicit Reader(std::span<const unsigned char> bytes) : bytes_(bytes) {}
    size_t size() const { return bytes_.size(); }
    std::span<const unsigned char> take(size_t count) {
        if(count>bytes_.size()) throw std::runtime_error("Truncated save data");
        auto value=bytes_.first(count);bytes_=bytes_.subspan(count);return value;
    }
    unsigned number() {
        auto bytes=take(4);
        return bytes[0] | (bytes[1]<<8) | (bytes[2]<<16) | (static_cast<unsigned>(bytes[3])<<24);
    }
    int int32() { return static_cast<int>(number()); }
    long long int64() {
        long long value{};
        std::memcpy(&value,take(8).data(),8);
        return value;
    }
    double float64() {
        double value{};
        std::memcpy(&value,take(8).data(),8);
        return value;
    }
    // length-prefixed block
    Reader region() {
        int length=int32();
        if(length<0) throw std::runtime_error("Invalid save region");
        return Reader(take(static_cast<size_t>(length)));
    }
    std::wstring text() {
        unsigned size=0;
        for(unsigned shift=0;;shift+=7) {
            if(shift>=28) throw std::runtime_error("Invalid save string length");
            auto b=take(1)[0];size|=static_cast<unsigned>(b&127)<<shift;
            if(!(b&128)) break;
        }
        if(size>4096) throw std::runtime_error("Save string too long");
        auto value=take(size);
        if(!size) return {};
        int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(value.data()),size,nullptr,0);
        if(!count) throw std::runtime_error("Invalid save text");
        std::wstring result(count,L'\0');
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(value.data()),size,result.data(),count);
        return result;
    }
private:
    std::span<const unsigned char> bytes_;
};

std::filesystem::path save_root() {
    PWSTR raw{};
    if(FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,0,nullptr,&raw)))
        throw std::runtime_error("Cannot locate game saves");
    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> folder(raw,CoTaskMemFree);
    return std::filesystem::path(raw)/L"Games By Malcs"/L"Melvor Idle 2"/L"steam";
}

std::vector<unsigned char> read_file(const std::filesystem::path& file) {
    std::ifstream input(file,std::ios::binary|std::ios::ate);
    if(!input) throw std::runtime_error("Cannot read Melvor save");
    auto size=input.tellg();
    if(size<=0 || size>16*1024*1024) throw std::runtime_error("Invalid save size");
    std::vector<unsigned char> compressed(static_cast<size_t>(size));
    input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(compressed.data()),size)) throw std::runtime_error("Save changed while reading");
    return compressed;
}

// whole=false stops after `limit` bytes (header only)
std::vector<unsigned char> decompress(const std::vector<unsigned char>& compressed, size_t limit, bool whole) {
    std::unique_ptr<BrotliDecoderState,decltype(&BrotliDecoderDestroyInstance)> decoder(
        BrotliDecoderCreateInstance(nullptr,nullptr,nullptr),BrotliDecoderDestroyInstance);
    if(!decoder) throw std::runtime_error("Cannot allocate save decoder");
    BrotliDecoderSetParameter(decoder.get(),BROTLI_DECODER_PARAM_LARGE_WINDOW,1);
    std::vector<unsigned char> output(whole ? std::min<size_t>(limit,compressed.size()*8) : limit);
    size_t remaining=compressed.size(), produced=0;
    const unsigned char* source=compressed.data();
    for(;;) {
        size_t free=output.size()-produced;
        unsigned char* destination=output.data()+produced;
        auto status=BrotliDecoderDecompressStream(decoder.get(),&remaining,&source,&free,&destination,nullptr);
        produced=output.size()-free;
        if(status==BROTLI_DECODER_RESULT_ERROR)
            throw std::runtime_error(std::string("Cannot decode save: ")+BrotliDecoderErrorString(BrotliDecoderGetErrorCode(decoder.get())));
        if(status==BROTLI_DECODER_RESULT_SUCCESS || !whole) break;
        // No final-block marker: consumed input is the end. Drain pending output first.
        if(status==BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && !BrotliDecoderHasMoreOutput(decoder.get())) {
            if(remaining) throw std::runtime_error("Truncated save");
            break;
        }
        if(output.size()>=limit) throw std::runtime_error("Save is larger than expected");
        output.resize(std::min(limit,output.size()*2));
    }
    if(!produced) throw std::runtime_error("Empty save");
    output.resize(produced);
    return output;
}

// Steam Early Access. 32 added a byte to each bank tab.
constexpr int oldest_save_version = 31;
constexpr int newest_save_version = 32;
bool known_version(int version) { return version>=oldest_save_version && version<=newest_save_version; }

// strict=false reads unknown versions; caller verifies (parse_progress)
ActivitySnapshot read_header(Reader& reader, bool strict, int& version) {
    version=reader.int32();
    if(strict && !known_version(version)) throw UnsupportedSaveVersion(version);
    ActivitySnapshot result;
    auto id = reader.take(result.save_id.size());
    std::copy(id.begin(), id.end(), result.save_id.begin());
    result.character=reader.text();
    reader.text(); // game mode
    result.save_ticks=static_cast<unsigned long long>(reader.int64()) & 0x3fffffffffffffffULL;
    result.skill=reader.text();
    reader.text(); // entity icon
    result.action=reader.text();
    reader.text(); // action icon
    return result;
}

std::wstring display_name(const std::wstring& id) {
    auto colon=id.rfind(L':');
    return colon==std::wstring::npos ? id : id.substr(colon+1);
}

// Bank inventory: header, then tabs of (quantity, flags, item ID). Summed across tabs.
void read_inventory(Reader inventory, int version, std::map<std::wstring, long long>& items) {
    inventory.int32(); // unknown, 0 so far
    int tabs=inventory.int32();
    if(tabs<0 || tabs>1000) throw std::runtime_error("Invalid bank tab count");
    for(int tab=0;tab<tabs;++tab) {
        inventory.take(4+4+4+1+1); // tab settings
        int count=inventory.int32();
        if(count<0 || count>100000) throw std::runtime_error("Invalid bank item count");
        for(int i=0;i<count;++i) {
            int quantity=inventory.int32();
            inventory.take(2); // placeholder, locked
            auto id=inventory.text();
            if(quantity>0) items[readable_name(id)]+=quantity;
        }
        inventory.text(); // tab name/icon
        int extras=inventory.int32();
        if(extras<0 || extras>100000) throw std::runtime_error("Invalid bank tab data");
        for(int i=0;i<extras;++i) { inventory.take(2); inventory.text(); }
        if(version>=32) inventory.take(1); // unknown, 0 so far
    }
}
}

std::wstring grouped(long long value) {
    auto digits=std::to_wstring(value<0 ? -value : value);
    for(int i=static_cast<int>(digits.size())-3;i>0;i-=3) digits.insert(static_cast<size_t>(i),L",");
    return digits;
}

std::wstring readable_name(const std::wstring& id) {
    auto name=display_name(id);
    std::wstring result;
    for(size_t i=0;i<name.size();++i) {
        wchar_t c=name[i];
        bool upper=iswupper(c)!=0;
        // "RawShrimp" -> "Raw Shrimp", "XPBoost" -> "XP Boost", digits stay ("2H")
        if(i>0 && upper && (iswlower(name[i-1])
                            || (iswupper(name[i-1]) && i+1<name.size() && iswlower(name[i+1])))) {
            // "BucketofSand", but not "Roof"
            size_t n=result.size();
            if(n>3 && result.ends_with(L"of") && iswlower(result[n-3]) && result[n-3]!=L'o') result.insert(n-2,L" ");
            result+=L' ';
        }
        result+=c==L'_' ? L' ' : c;
    }
    return result;
}

std::wstring ActivitySnapshot::label() const {
    if(skill.empty() && action.empty()) return L"Idle";
    if(skill.empty()) return action;
    return action.empty() ? skill : skill+L" · "+action;
}
ActivitySnapshot parse_activity_header(std::span<const unsigned char> bytes) {
    Reader reader(bytes);
    int version{};
    return read_header(reader,true,version);
}
ActivitySnapshot read_activity_save(const std::filesystem::path& file) {
    // header only
    auto decoded=decompress(read_file(file),32*1024,false);
    return parse_activity_header(decoded);
}

long long xp_for_level(int level) {
    // RuneScape XP curve. Levels from a real save add up to its stored total.
    double points=0;
    for(int n=1;n<level;++n) points+=std::floor(n+300*std::pow(2.0,n/7.0));
    return static_cast<long long>(std::floor(points/4));
}
int level_for_xp(double xp) {
    int level=1;
    while(level<max_level && xp>=static_cast<double>(xp_for_level(level+1))) ++level;
    return level;
}

ProgressSnapshot parse_progress(std::span<const unsigned char> bytes) {
    Reader reader(bytes);
    ProgressSnapshot result;
    int version{};
    result.activity=read_header(reader,false,version);
    result.total_level=reader.int32();
    result.gp=reader.int64();
    result.slayer_coins=reader.int64();
    reader.take(1); // demo flag
    // named sections; skills hold Experience (double XP)
    int sections=reader.int32();
    if(sections<0 || sections>100000) throw std::runtime_error("Invalid save section count");
    for(int i=0;i<sections;++i) {
        auto name=reader.text();
        auto section=reader.region();
        // skip sections that don't parse; the length prefix says where the next starts
        try {
            int entries=section.int32();
            if(entries<0 || entries>100000) continue;
            for(int j=0;j<entries;++j) {
                auto key=section.text();
                auto value=section.region();
                if(key==L"Player" && name==L"MelvorBase:Combat") {
                    auto health=value.region();
                    if(health.size()==4) result.health=health.int32();
                    continue;
                }
                if(key==L"Statistics" && (name==L"MelvorBase:Combat" || name==L"MelvorBase:Thieving")) {
                    // same layout, counters are doubles
                    try {
                        while (value.size()) {
                            int stats=value.int32();
                            if (stats<0 || stats>10000) throw std::runtime_error("Invalid statistics group");
                            for(int k=0;k<stats;++k) {
                                auto stat=value.text();
                                auto number=value.region();
                                if(stat==L"MelvorBase:CombatPlayerDeaths" && number.size()==8)
                                    result.deaths=static_cast<long long>(number.float64());
                                if(stat==L"MelvorBase:ThievingDamageTaken" && name==L"MelvorBase:Thieving") {
                                    int count=number.int32();
                                    if(count<0 || count>100000) continue;
                                    double total=0;
                                    for(int entry=0;entry<count;++entry) {
                                        double damage=number.float64();
                                        if(!std::isfinite(damage) || damage<0) throw std::runtime_error("Invalid Thieving damage");
                                        total+=damage;
                                        number.text(); // target ID
                                    }
                                    if(number.size()==0 && std::isfinite(total)) result.thieving_damage=total;
                                }
                            }
                        }
                    } catch(const std::runtime_error&) {} // optional
                    continue;
                }
                if(key==L"Inventory" && display_name(name)==L"Bank") {
                    try { read_inventory(value,version,result.items); result.bank_read=true; }
                    catch(const std::runtime_error&) { result.items.clear(); }
                    continue;
                }
                // v31 stores XP only; some builds add the level
                if(key==L"Experience" && (value.size()==8 || value.size()==12)) {
                    SkillProgress skill;
                    skill.xp=value.float64();
                    skill.level=value.size()==12 ? value.int32() : level_for_xp(skill.xp);
                    result.skills[display_name(name)]=skill;
                }
            }
        } catch(const std::runtime_error&) {
            continue;
        }
    }
    // Unknown version: accept only if the skill levels add up to the stored total.
    if(!known_version(version)) {
        int sum=0;
        for(const auto& [name,skill]:result.skills) sum+=skill.level;
        if(result.skills.size()<10 || sum!=result.total_level) throw UnsupportedSaveVersion(version);
    }
    return result;
}

UnsupportedSaveVersion::UnsupportedSaveVersion(int version)
    : std::runtime_error("Unsupported Melvor save version "+std::to_string(version)), version_(version) {}

std::vector<unsigned char> decode_save(const std::filesystem::path& file) {
    return decompress(read_file(file),256*1024*1024,true);
}

ProgressSnapshot read_progress_save(const std::filesystem::path& file) {
    auto before=std::filesystem::last_write_time(file);
    auto decoded=decode_save(file);
    auto result=parse_progress(decoded);
    result.source=file;
    result.saved_at=std::filesystem::last_write_time(file);
    if(result.saved_at!=before) throw std::runtime_error("Save is being updated");
    return result;
}

std::optional<SaveFile> newest_save() {
    std::optional<SaveFile> newest;
    auto root=save_root();
    if(!std::filesystem::exists(root)) return newest;
    for(const auto& account:std::filesystem::directory_iterator(root)) {
        if(!account.is_directory()) continue;
        auto folder=account.path()/L"saves";
        if(!std::filesystem::exists(folder)) continue;
        for(const auto& entry:std::filesystem::directory_iterator(folder)) {
            if(entry.path().extension()!=L".sav" || !entry.is_regular_file()) continue;
            auto changed=entry.last_write_time();
            if(!newest || changed>newest->modified) newest=SaveFile{entry.path(),changed};
        }
    }
    return newest;
}

ProgressChange compare_progress(const ProgressSnapshot& before, const ProgressSnapshot& after) {
    ProgressChange change;
    for(const auto& [name,now]:after.skills) {
        auto old=before.skills.find(name);
        double xp_before=old==before.skills.end() ? 0 : old->second.xp;
        int level_before=old==before.skills.end() ? now.level : old->second.level;
        if(now.xp-xp_before<1) continue;
        double next=now.level<max_level ? static_cast<double>(xp_for_level(now.level+1))-now.xp : 0;
        change.skills.push_back({name,now.xp-xp_before,level_before,now.level,std::max(0.0,next)});
    }
    std::sort(change.skills.begin(),change.skills.end(),[](const SkillGain& a,const SkillGain& b) { return a.xp>b.xp; });
    // missing = zero, but an unreadable bank is not an empty one
    std::map<std::wstring,long long> delta;
    if(before.bank_read && after.bank_read) {
        for(const auto& [name,count]:after.items) delta[name]+=count;
        for(const auto& [name,count]:before.items) delta[name]-=count;
    }
    for(const auto& [name,count]:delta) {
        if(count>0) change.gained.push_back({name,count});
        if(count<0) change.used.push_back({name,count});
    }
    std::sort(change.gained.begin(),change.gained.end(),[](const ItemChange& a,const ItemChange& b) { return a.change>b.change; });
    std::sort(change.used.begin(),change.used.end(),[](const ItemChange& a,const ItemChange& b) { return a.change<b.change; });
    change.gp=after.gp-before.gp;
    change.slayer_coins=after.slayer_coins-before.slayer_coins;
    if(before.deaths && after.deaths && *after.deaths>*before.deaths) change.deaths=*after.deaths-*before.deaths;
    change.total_level_from=before.total_level;
    change.total_level_to=after.total_level;
    change.between_saves=std::chrono::duration_cast<std::chrono::seconds>(after.saved_at-before.saved_at);
    return change;
}

std::wstring duration_text(std::chrono::seconds elapsed) {
    auto seconds=std::max<long long>(0,elapsed.count());
    auto minutes=seconds/60;
    if(minutes>=60) return std::format(L"{}h {}m",minutes/60,minutes%60);
    return minutes ? std::format(L"{}m",minutes) : std::format(L"{}s",seconds);
}

std::wstring death_text(const ProgressChange& change) {
    std::wstring text=change.deaths==1 ? std::wstring(L"Your character died") : std::format(L"Your character died {} times",change.deaths);
    if(change.possible_thieving_death) text=L"Possible Thieving death";
    text += L".\nIdle after death while hidden: ";
    text += change.idle_after_death ? duration_text(*change.idle_after_death) : L"timing unavailable";
    return text;
}

std::wstring describe_progress(const ProgressSnapshot& before, const ProgressSnapshot& after, size_t max_skills) {
    return describe_change(compare_progress(before,after),max_skills);
}

std::wstring describe_change(const ProgressChange& change, size_t max_skills) {
    std::wstring text;
    auto line=[&](const std::wstring& value) { if(!text.empty()) text+=L"\n"; text+=value; };
    // death first
    if(change.deaths>0) line(death_text(change));
    for(size_t i=0;i<change.skills.size() && i<max_skills;++i) {
        const auto& gain=change.skills[i];
        auto entry=std::format(L"{} +{} XP",gain.name,grouped(static_cast<long long>(gain.xp)));
        if(gain.to>gain.from) entry+=std::format(L" ({} \u2192 {})",gain.from,gain.to);
        line(entry);
    }
    if(change.skills.size()>max_skills) line(std::format(L"+{} more skills",change.skills.size()-max_skills));
    auto currency=[&](long long amount,const wchar_t* unit) {
        if(amount) line(std::format(L"{}{} {}",amount>0 ? L"+" : L"\u2212",grouped(amount),unit));
    };
    currency(change.gp,L"GP");
    currency(change.slayer_coins,L"slayer coins");
    return text;
}

void GameActivity::refresh() noexcept {
    auto now=std::chrono::steady_clock::now();
    if(now<next_check_) return;
    next_check_=now+std::chrono::seconds(5);
    try {
        auto newest=newest_save();
        if(!newest) {available_=false;error_=L"No local save found";return;}
        if(available_ && newest->path==source_ && newest->modified==modified_) return;
        if(!available_ && newest->path==source_ && newest->modified==modified_) return; // same failing save
        source_=newest->path;
        modified_=newest->modified;
        ActivitySnapshot result;
        try {
            result=read_activity_save(newest->path);
        } catch(const UnsupportedSaveVersion&) {
            // newer format: full read, verified
            result=read_progress_save(newest->path).activity;
        }
        if(std::filesystem::last_write_time(newest->path)!=newest->modified) throw std::runtime_error("Save is being updated");
        snapshot_=std::move(result);available_=true;
    } catch(const UnsupportedSaveVersion& error) {
        available_=false;
        error_=std::format(L"Save version {} isn't supported yet. Check for a Melvor Companion update.",error.version());
        strip_error_=L"Update needed";
    } catch(...) {
        available_=false;
        error_=L"Save unavailable or unsupported";
        strip_error_=L"Activity unavailable";
        modified_={}; // retry next check
    }
}
std::wstring GameActivity::strip() const {
    return available_ ? snapshot_.label() : strip_error_;
}
std::wstring GameActivity::details() const {
    if(!available_) return error_;
    auto age=std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now()-modified_).count();
    return std::format(L"{}: {}\nLast saved {}s ago",snapshot_.character,snapshot_.label(),(std::max)(decltype(age)(0),age));
}
}
