#include "death_panel.hpp"
#include <algorithm>
#include <array>
#include <vector>
#include <string_view>

namespace companion {
namespace {
bool white(std::uint32_t p) {
    return (p & 255) >= 200 && ((p >> 8) & 255) >= 200 && ((p >> 16) & 255) >= 200;
}
bool red(std::uint32_t p) {
    auto r = (p >> 16) & 255;
    return r > 120 && r > ((p >> 8) & 255) * 1.65 && r > (p & 255) * 1.65;
}

// sampled points across the YOU DIED lettering: white letters plus red outline
constexpr std::array<std::string_view, 7> rows{
    ".+.-r+rr+r++-r++rr+r--rr+.++r-r+rr++..+r++.++-",
    "-+.-rr.+r--+r-++--rr---r+r-++-r+r-.+--+-.+--+r",
    "-r+-+-r+r--++-++--rr---r+r-r+.r+r-.+----.+--++",
    "--+rr-r+.--r+-++--rr---r+r-r+rr+r-r+++.-.+--++",
    "--++--r+r--++-++--rr---r+r-.+-r+r-.+----.+--r+",
    "--++---+r-.+r-++--+r---r+r-++-r+r-.+--+-.+-.+r",
    "-++++--r+++.--r++++---r+++++--+++r+++++.+++++-"
};

bool matches(std::span<const std::uint32_t> frame, int stride, int left, int top, int width, int height) {
    if (left + width >= stride || top + height >= static_cast<int>(frame.size()/stride)) return false;
    int whites{}, reds{}, white_hits{}, red_hits{};
    for (int y = 0; y < 7; ++y) {
        const int py = top + (2*y+1)*height/14;
        for (int x = 0; x < static_cast<int>(rows[y].size()); ++x) {
            const int px = left + (2*x+1)*width/(2*static_cast<int>(rows[y].size()));
            const auto pixel = frame[py*stride+px];
            if (rows[y][x] == '+') { ++whites; white_hits += white(pixel); }
            if (rows[y][x] == 'r') { ++reds; red_hits += red(pixel); }
        }
    }
    return white_hits*100 >= whites*79 && red_hits*100 >= reds*65;
}
}

bool has_death_banner(std::span<const std::uint32_t> frame, int width, int height) {
    if (width < 100 || height < 16 || width > 8192 || height > 8192 ||
        frame.size() != static_cast<size_t>(width)*height) return false;
    // scale from the Y, then check the whole phrase so a lone glyph can't pass
    std::vector<unsigned char> mask(frame.size());
    std::transform(frame.begin(), frame.end(), mask.begin(), white);
    std::vector<int> stack;
    for (int pos = 0; pos < static_cast<int>(mask.size()); ++pos) {
        if (!mask[pos]) continue;
        int x0=pos%width, x1=x0, y0=pos/width, y1=y0;
        mask[pos]=0;
        stack.push_back(pos);
        auto visit = [&](int index) {
            if (mask[index]) { mask[index]=0; stack.push_back(index); }
        };
        while (!stack.empty()) {
            int i=stack.back(); stack.pop_back();
            int x=i%width, y=i/width;
            x0=std::min(x0,x); x1=std::max(x1,x); y0=std::min(y0,y); y1=std::max(y1,y);
            if (x) visit(i-1);
            if (x+1<width) visit(i+1);
            if (y) visit(i-width);
            if (y+1<height) visit(i+width);
        }
        int w=x1-x0+1, h=y1-y0+1;
        if (h<16 || w*100<h*86 || w*100>h*109) continue;
        // rounding one letter scales the error by seven, so try nearby scales
        for (int offset=-8; offset<=8; offset+=2) {
            if (matches(frame,width,x0,y0,w*572/79+offset,h)) return true;
        }
    }
    return false;
}

bool window_has_death_banner(HWND window, DWORD expected_process) {
    DWORD pid{};
    RECT rect{};
    if (!GetWindowThreadProcessId(window,&pid) || pid!=expected_process ||
        !GetClientRect(window,&rect) || rect.right<100 || rect.bottom<16 ||
        rect.right>8192 || rect.bottom>8192 || static_cast<long long>(rect.right)*rect.bottom>16777216)
        return false;
    HDC dc=CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO info{};
    info.bmiHeader={sizeof(BITMAPINFOHEADER),rect.right,-rect.bottom,1,32,BI_RGB};
    void* bits{};
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    bool found=false;
    if (bitmap) {
        auto previous=SelectObject(dc,bitmap);
        if (previous && previous!=HGDI_ERROR && PrintWindow(window,dc,PW_CLIENTONLY|PW_RENDERFULLCONTENT)) {
            GdiFlush();
            try {
                found=has_death_banner({static_cast<const std::uint32_t*>(bits),
                    static_cast<size_t>(rect.right)*rect.bottom},rect.right,rect.bottom);
            } catch (const std::bad_alloc&) { /* A missed observation can be retried. */ }
        }
        if (previous && previous!=HGDI_ERROR) SelectObject(dc,previous);
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    return found;
}
}
