// ftxui_impl.cpp : compile FTXUI (third_party/ftxui, version amalgamée) dans sa propre unité.
// FTXUI exige UNICODE et NOMINMAX sous Windows ; les définir ici évite des options de compilation.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../third_party/ftxui/ftxui.cpp"
