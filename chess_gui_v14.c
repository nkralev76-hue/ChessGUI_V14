/*
 * Chess GUI v14 — CMD Edition — SDL2 graphical chess with Dual UCI Engine Support
 * Based on chess_fixed.c (SDL2 GUI) + uci_chess_v8.2.c (UCI engine)
 * Engine features: PVS, Razoring, ProbCut, SEE, LMR table, pawn hash,
 *   continuation history, countermove history, capture history (v8.3),
 *   king safety, pin detection, trapped bishop, battery bonus, king tropism,
 *   outpost bonus, TT with age, aggressive LMR, tempo bonus (v8.3)
 * GUI features: SDL2 rendering, piece images, sound, clocks, pondering,
 *   move history, PGN export, opening book, themes, promo popup
 * v12: captured-pieces tray + material advantage, piece slide
 *   animation, drag-and-drop moving, eval-history sparkline, opening-name
 *   recognizer, soft piece shadows
 * v13 CMD: Completely new CMD-style interface — black/white/gray/orange
 *   with red/blue accents, minimal flat design, no shadows, separate
 *   per-engine analysis panels with names, clear Ponder ON/OFF indicator.
 *   V12 remains untouched.
 * v14: Real Tournament Manager (Tournament -> Tournament Manager...) — add
 *   any number of engines (or the built-in) to a roster, auto round-robin
 *   (single/double) with live standings (Pts/W/D/L/Sonneborn-Berger) and
 *   standings export. UCI Options dialog: left-click a Spin row now opens
 *   exact numeric entry (e.g. Threads=2) instead of only coarse stepping —
 *   works for any option an external engine reports, including SyzygyPath
 *   (string) and Threads (spin). The built-in engine is unchanged: no
 *   threads, no tablebases — only external UCI engines use those settings.
 * Compile: gcc -O3 -march=native -flto -funroll-loops -mpopcnt -mavx2
 *          -o chess_gui_v14 chess_gui_v14.c -lSDL2 -lSDL2_image -lm -lcomdlg32
 */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <ctype.h>
/* Platform headers for UCI engine process management */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>   /* GetOpenFileName */
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <fcntl.h>
#  include <sys/select.h>
#  include <dirent.h>
#  include <sys/stat.h>
#endif
#ifdef USE_BOOK
#include "book.h"
#endif

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6
#define WHITE  1
#define BLACK -1
#define CR_WK 1
#define CR_WQ 2
#define CR_BK 4
#define CR_BQ 8

typedef uint64_t BB;
#define BB_SET(b,s)  ((b) |=  (1ULL<<(s)))
#define BB_CLR(b,s)  ((b) &= ~(1ULL<<(s)))
#define BB_GET(b,s)  (((b)>>(s))&1ULL)
#define BB_LSB(b)    ((int)__builtin_ctzll(b))
#define BB_POP(b)    ((b)&=(b)-1)

#define BB_P 0
#define BB_N 1
#define BB_B 2
#define BB_R 3
#define BB_Q 4
#define BB_K 5
#define WC 0
#define BC 1

typedef struct {
    BB pieces[2][6];
    BB occ[3];
    int cr, ep, fifty;
    uint64_t hash;
    uint64_t pawn_hash;
} BBoard;

typedef struct {
    int fr, fc, tr, tc, cap, promo, castle, ep_cap;
    int score;
} Move;

/* ---- Layout ---- */
static int SQ_SIZE = 44; /* v14.2: slightly smaller for equal gaps (was 48) */
static int real_w = 0, real_h = 0;
#define BRD      (SQ_SIZE*8)          /* pure board pixel size          */
#define MENU_H   38
#define CLOCK_BAR_H 32                /* v13 CMD: clocks above board */
#define SIDE_W   480                  /* v14: wider for 1.2x text (was 420) */
#define COORD_W  28                   /* rank labels strip (left)        */
#define COORD_H  22                   /* file labels strip (bottom)      */
#define BPAD      6                   /* padding inside wood frame       */
#define BOARD_GAP 14                  /* v14.2: equal gap board<->clocks and board<->tabs */
#define BOARD_OX (COORD_W+BOARD_GAP)       /* pixel X where sq(0,0) starts    */
#define BOARD_OY (MENU_H+CLOCK_BAR_H+BOARD_GAP) /* board below clock bar */
#define FRAME_W  (COORD_W+BOARD_GAP+BRD+BOARD_GAP) /* wood frame + coords width   */
#define FRAME_H  (CLOCK_BAR_H+BOARD_GAP+BRD+COORD_H+BOARD_GAP) /* includes clock bar + equal gaps */
#define LOG_H    150                  /* v13 CMD: much taller bottom log — request */
#define PANEL_W  (SIDE_W-8)           /* uniform panel width */
#define PANEL_H  105                  /* uniform panel height — larger per request, was 88 */
#define WIN_W    (FRAME_W+SIDE_W)
#define WIN_H    (MENU_H+FRAME_H+LOG_H)
static int bottom_log_tab = 0; /* 0=Engines, 1=Log */
#define BOTTOM_LOG_MAX 200
static char bottom_log_lines[BOTTOM_LOG_MAX][128]; static int bottom_log_n=0;
static void bottom_log_push(const char *s){ if(bottom_log_n<BOTTOM_LOG_MAX){ strncpy(bottom_log_lines[bottom_log_n],s,127); bottom_log_lines[bottom_log_n][127]=0; bottom_log_n++; } else { for(int i=1;i<BOTTOM_LOG_MAX;i++) strcpy(bottom_log_lines[i-1],bottom_log_lines[i]); strncpy(bottom_log_lines[BOTTOM_LOG_MAX-1],s,127); } }

/* which engine slot a log line belongs to: 0 = Engine 1 (E1), 1 = Engine 2
   (E2), -1 = neither (game/status messages). Only true UCI-protocol lines
   (RECV/SEND/ENGINE/PART/CRASH/PONDER*) carry a per-engine slot and belong in
   Out1/Out2; everything else (GAME/CLOCK/WARN/TAB/LOG/...) is a status line for
   the Log tab. Matching on the bracket keyword (not just the number) avoids
   misrouting lines like "[TAB -> Out1]" into an engine tab. */
static int bottom_log_engine(const char *ln){
    const char *dirs[] = {"[RECV ","[SEND ","[ENGINE ","[PART ","[CRASH ",
                          "[PONDER ","[PONDER-HIT ","[PONDER-BEST "};
    for(int k=0;k<8;k++){
        int L=(int)strlen(dirs[k]);
        if(!strncmp(ln,dirs[k],L)){
            int e = atoi(ln+L);
            return e==1?1:0;
        }
    }
    return -1;
}

/* v13: copy the VISIBLE bottom tab to the system clipboard (key C) — Out1
   copies Engine 1's UCI traffic, Out2 copies Engine 2's, Log copies the
   game/status messages. So "C" copies what you are actually looking at. */
static void copy_log_to_clipboard(void){
    int want_ei = (bottom_log_tab==0)?0 : (bottom_log_tab==1)?1 : -1;
    int cnt=0;
    for(int i=0;i<bottom_log_n;i++){
        if(bottom_log_engine(bottom_log_lines[i])==want_ei) cnt++;
    }
    if(cnt==0){ bottom_log_push("LOG: nothing to copy"); return; }
    size_t cap=(size_t)cnt*130+16; char *buf=malloc(cap);
    if(!buf){ bottom_log_push("LOG: copy failed (out of memory)"); return; }
    buf[0]=0;
    for(int i=0;i<bottom_log_n;i++){
        if(bottom_log_engine(bottom_log_lines[i])==want_ei){
            strncat(buf,bottom_log_lines[i],cap-1); strncat(buf,"\n",2);
        }
    }
    if(SDL_SetClipboardText(buf)==0) bottom_log_push("LOG: copied to clipboard");
    else bottom_log_push("LOG: clipboard copy failed");
    free(buf);
}

/* ---- AI constants ---- */
#define TT_SIZE (1 << 20)
#define INF     30000
#define MATE    29000
#define MAX_DEPTH 99
#define MAX_PLY  48

/* ---- Board themes — v13 CMD: flat black/gray/orange palette — blue removed per request ---- */
typedef struct { int lr,lg,lb,dr,dg,db; const char *name; } Theme;
static const Theme THEMES[]={
    {184,184,184,  58, 58, 58, "CMD Gray"},
    {210,210,210,  42, 42, 42, "CMD High Contrast"},
    {212,190,140,  62, 52, 32, "CMD Amber"},
    {200,180,180,  68, 44, 44, "CMD Red Tint"},
    {235,235,210,115,150,115, "Classic Green"},
    {240,217,181,181,136, 99, "Classic Brown"},
    {230,230,230,140,140,140, "Classic Gray"},
    {185,205,235,  46, 66,110, "CMD Blue"},
};
#define NUM_THEMES (sizeof(THEMES)/sizeof(THEMES[0]))
static int cur_theme=0; /* default CMD Gray */
/* ---- CMD visual toggles (v13) — kept minimal, no shadows/big coords ---- */
static int retro_scanlines = 0; /* off in CMD — optional very faint grid */
static int retro_vignette  = 0;
static int retro_crt_curve = 0;
static int piece_shadows   = 0; /* user requested: no shadows */
static int retro_coords_big = 0; /* user requested: no big coords */
static int retro_piece_style = 0;
/* v14: UI text scale — 1.2 requested for readability */
static double UI_TEXT_SCALE = 1.2;

/* ---- GUI state ---- */
BBoard B;
int turn=WHITE,sel_r=-1,sel_c=-1,game_over=0,player_color=WHITE;
int lm_fr=-1,lm_fc=-1,lm_tr=-1,lm_tc=-1;
int promo_pending=0,promo_fr,promo_fc,promo_tr,promo_tc;
int flip_board=0,sound_on=1,aivsai=0,draw_offered=0,use_book=1,use_ponder=1,both_human=0;

char msg[256]="Your move (White)";
Uint32 clk_w,clk_b,last_ms;
Uint32 base_time = 5*60*1000;
Uint32 increment = 0;
int time_control_type = 0;
/* v12.9: the clocks stay frozen until the FIRST move of the game is played.
   Without this, when the CPU plays White the clock starts counting down
   during the engine's first think, so a selected 3-min game visibly
   "starts" at ~2:48 and a 5-min one at ~4:xx (on slow machines the first
   search iteration alone can eat 10-60s). Now the clock always shows the
   exact selected time at game start and only starts ticking after move 1. */
int clock_started = 0;

int cap_w[7],cap_b[7];
typedef struct { Move m; BBoard bb; int turn; uint64_t hash; } Hist;
#define MAX_HIST 400
Hist hist[MAX_HIST];int hist_n=0;
/* PGN replay */
static int pgn_replay_mode=0;
static int pgn_replay_index=0;
static Move *pgn_replay_moves=NULL;
static int pgn_replay_move_count=0;
static int pgn_replay_auto=0;
static Uint32 pgn_replay_auto_last=0;
#define PGN_REPLAY_DELAY 700
int open_menu=-1;
static Uint32 menu_outside_t0=0; /* v13.1: grace timer so the dropdown doesn't vanish while the mouse travels to it */

/* v12: slide animation for moved pieces */
int anim_active=0;
Uint32 anim_start=0;
#define ANIM_MS 140
int anim_piece=0;
int anim_from_r=-1,anim_from_c=-1,anim_to_r=-1,anim_to_c=-1;

/* v12: drag-and-drop */
int drag_active=0,drag_r=-1,drag_c=-1,drag_piece=0,drag_mx=0,drag_my=0;

/* v12: eval history for sidebar sparkline (indexed like hist[]) */
#define EVAL_HIST_MAX MAX_HIST
int eval_hist[EVAL_HIST_MAX];

SDL_Window *win=NULL;
SDL_Renderer *ren=NULL;

/* ===================== v14: TTF TEXT RENDERING =====================
   Replaces the old 8x9 bitmap font (blocky at any non-1x scale, no
   anti-aliasing) with a real TrueType font rendered through SDL_ttf.
   font_raw  = base-size font, used by dtxt_raw() (always drawn at sc=1,
               independent of UI_TEXT_SCALE — menus, logs, dialogs).
   font_ui   = UI_TEXT_SCALE-sized font, used by dtxt() (panel labels,
               eval bar, captured pieces, move list, etc).
   RAW_ADV / UI_ADV hold each font's *average* character advance width in
   pixels (measured once at load time). The rest of the file estimates
   string pixel widths for truncation/centering with "strlen(s)*ADV"
   instead of doing a full TTF_SizeUTF8() at every call site — this was
   already the pattern used with the old fixed 9px-per-char bitmap font,
   just now driven by a real (proportional-average) measurement instead
   of a hardcoded constant. It's an approximation for genuinely
   proportional text, but keeps every existing layout call site working
   with a one-line change instead of a full rewrite. */
static TTF_Font *font_raw = NULL;
static TTF_Font *font_ui  = NULL;
static double RAW_ADV = 9.0, UI_ADV = 9.0; /* fallback matches old bitmap font */
static int RAW_LINE_H = 9, UI_LINE_H = 9;

#define GLYPH_CACHE_SIZE 1024
typedef struct { char key[192]; SDL_Texture *tex; int w,h; int used; } GlyphCacheEnt;
static GlyphCacheEnt glyph_cache[GLYPH_CACHE_SIZE];

static unsigned glyph_hash(const char*s){
    unsigned h=2166136261u;
    for(;*s;s++){ h^=(unsigned char)*s; h*=16777619u; }
    return h;
}
/* Renders (or fetches from cache) a texture for `text` in font `f` and
   color R,Gv,B, then draws it at x,y. Cache key includes font identity so
   font_raw and font_ui never collide. */
static void ttf_draw(TTF_Font*f,int is_ui,int x,int y,const char*t,int R,int Gv,int B){
    if(!f || !t || !t[0]) return;
    char key[192];
    snprintf(key,sizeof(key),"%d|%d,%d,%d|%s",is_ui,R,Gv,B,t);
    unsigned h = glyph_hash(key) % GLYPH_CACHE_SIZE;
    unsigned probe = h;
    for(int tries=0; tries<8; tries++, probe=(probe+1)%GLYPH_CACHE_SIZE){
        GlyphCacheEnt*e=&glyph_cache[probe];
        if(e->used && strcmp(e->key,key)==0){
            SDL_Rect d={x,y,e->w,e->h};
            SDL_RenderCopy(ren,e->tex,NULL,&d);
            return;
        }
    }
    /* miss: render fresh, evict the first probed slot (simple, bounded cache) */
    SDL_Color col={ (Uint8)R,(Uint8)Gv,(Uint8)B,255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f,t,col);
    if(!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren,surf);
    int w=surf->w,hh=surf->h;
    SDL_FreeSurface(surf);
    if(!tex) return;
    GlyphCacheEnt*slot=&glyph_cache[h];
    if(slot->used && slot->tex) SDL_DestroyTexture(slot->tex);
    strncpy(slot->key,key,sizeof(slot->key)-1); slot->key[sizeof(slot->key)-1]=0;
    slot->tex=tex; slot->w=w; slot->h=hh; slot->used=1;
    SDL_Rect d={x,y,w,hh};
    SDL_RenderCopy(ren,tex,NULL,&d);
}
static void init_fonts(void){
    if(TTF_Init()!=0) return;
    int base_pt = 13;                                   /* tune to taste */
    int ui_pt   = (int)round(base_pt*UI_TEXT_SCALE); if(ui_pt<base_pt) ui_pt=base_pt;
    font_raw = TTF_OpenFont("fonts/DejaVuSans.ttf", base_pt);
    font_ui  = TTF_OpenFont("fonts/DejaVuSans.ttf", ui_pt);
    const char*sample="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    if(font_raw){
        int w,h; TTF_SizeUTF8(font_raw,sample,&w,&h);
        RAW_ADV=(double)w/(double)strlen(sample); RAW_LINE_H=TTF_FontHeight(font_raw);
    }
    if(font_ui){
        int w,h; TTF_SizeUTF8(font_ui,sample,&w,&h);
        UI_ADV=(double)w/(double)strlen(sample); UI_LINE_H=TTF_FontHeight(font_ui);
    }
}

SDL_AudioDeviceID aud=0;
SDL_Texture *tex[2][7];

/* ===================== ENGINE DATA ===================== */
typedef struct {
    uint64_t key;
    int depth, score, flag;
    uint8_t age;
    Move move;
} TTEntry;
static TTEntry transposition_table[TT_SIZE];

typedef struct { uint64_t key; int score; BB passed[2]; } PawnTTEntry;
#define PAWN_TT_SIZE (1 << 16)
#define PAWN_TT_MASK (PAWN_TT_SIZE - 1)
static PawnTTEntry pawn_tt[PAWN_TT_SIZE];

static uint64_t zobrist_pieces[12][64];
static uint64_t zobrist_side;
static uint64_t zobrist_cr[4];
static uint64_t zobrist_ep[8];

static BB bb_knight_att[64];
static BB bb_king_att[64];
static BB bb_pawn_att[2][64];
#define MG_ROOK_TBL   4096
#define MG_BISH_TBL    512
static BB mg_rook_mask[64], mg_bish_mask[64];
static BB mg_rook_magic[64], mg_bish_magic[64];
static int mg_rook_shift[64], mg_bish_shift[64];
static BB mg_rook_att[64][MG_ROOK_TBL];
static BB mg_bish_att[64][MG_BISH_TBL];

/* ---- Engine globals ---- */
static uint32_t start_time, target_time, hard_limit;
static _Atomic int stop_search = 0;
static long long nodes_count;
static uint8_t tt_age = 1;

static int history[2][64][64];
/* v8.3: Capture history for improved ordering of capture moves */
static int cap_history[12][64];
static Move killer[MAX_PLY][2];
static int killer_cnt[MAX_PLY];
static Move counter[2][64][64];
static int cmh[12][64];
static int cont_hist[12][12][64];

static uint64_t line_hashes[MAX_PLY];
#define MAX_GAME_HIST 1024
static uint64_t game_hist_hashes[MAX_GAME_HIST];
static int game_hist_n = 0;

static _Atomic int ai_cancel = 0;
static _Atomic int ai_thinking = 0, ai_done = 0;
static int ai_is_ponder = 0;
static int analysis_mode = 0;  /* v8.3.1: infinite analysis toggle (I key) */
static Move ai_result, g_best;
static int g_best_eval;
static int g_best_depth = 0;
static long long g_nps = 0;
static char g_pv_str[512]="";

/* v13: per-engine analysis panels with names */
typedef struct {
    char name[128];
    int depth;
    int eval; /* centipawns, White perspective */
    char pv[512];
    long long nps;
    long long nodes;
    int has_data;
    int is_thinking;
} EngAnalysis;
#define ENG_BUILTIN_IDX 2
static EngAnalysis eng_analysis[3]; /* 0=UCI1,1=UCI2,2=Built-in */
static void eng_analysis_update(int idx, const char *name, int depth, int eval, const char *pv, long long nps, long long nodes, int thinking){
    if(idx<0||idx>=3) return;
    if(name && name[0]) { strncpy(eng_analysis[idx].name, name, 127); eng_analysis[idx].name[127]=0; }
    eng_analysis[idx].depth = depth;
    eng_analysis[idx].eval = eval;
    if(pv){ strncpy(eng_analysis[idx].pv, pv, 511); eng_analysis[idx].pv[511]=0; }
    eng_analysis[idx].nps = nps;
    eng_analysis[idx].nodes = nodes;
    eng_analysis[idx].has_data = 1;
    eng_analysis[idx].is_thinking = thinking;
}


/* ===================== v10: UCI ENGINE SLOTS ===================== */
typedef enum { UOPT_CHECK, UOPT_SPIN, UOPT_COMBO, UOPT_STRING, UOPT_BUTTON } UCIOptType;

typedef struct {
    char name[128];
    UCIOptType type;
    /* check */
    int def_check, cur_check;
    /* spin */
    int def_spin, cur_spin, spin_min, spin_max;
    /* string */
    char def_str[256], cur_str[256];
    /* combo */
    char combo_vars[64][128];
    int combo_count;
    int combo_def_idx, combo_cur_idx;
} UCIOption;

#define MAX_UCI_OPTIONS 128
#define MAX_COMBO_VARS 64

typedef struct {
    char path[256];
    char name[128];
    int ready;
    UCIOption options[MAX_UCI_OPTIONS];
    int num_options;
} UCIEngineData;

#define MAX_ENGINES 2
static UCIEngineData uci_eng[MAX_ENGINES];
static int active_engine = 0;

static int   use_uci_engine = 0;   /* 0 = built-in, 1 = external UCI */

/* Platform engine I/O — per slot */
#ifdef _WIN32
static HANDLE uci_hproc[MAX_ENGINES] = {NULL,NULL};
static HANDLE uci_hin[MAX_ENGINES]   = {NULL,NULL};
static HANDLE uci_hout[MAX_ENGINES]  = {NULL,NULL};
#define UCI_VALID(idx) (uci_hproc[idx] != NULL)
#else
static pid_t  uci_pid[MAX_ENGINES]     = {-1,-1};
static int    uci_to_fd[MAX_ENGINES]   = {-1,-1};
static int    uci_from_fd[MAX_ENGINES] = {-1,-1};
static FILE  *uci_wf[MAX_ENGINES]      = {NULL,NULL};
static FILE  *uci_rf[MAX_ENGINES]      = {NULL,NULL};
#define UCI_VALID(idx) (uci_pid[idx] > 0)
#endif

/* Move that the UCI thread places its result in */
static Move  uci_result;
static _Atomic int uci_done = 0;

/* FEN of position from which the current game started (empty = standard) */
static char  game_start_fen[256] = "";

/* ===================== v10: TOURNAMENT ===================== */
static int   tourney_active    = 0;
static int   tourney_total     = 10;   /* total games to play       */
static int   tourney_played    = 0;
static float tourney_score[2]  = {0,0}; /* [0]=white side, [1]=black side */
static int   tourney_delay_ms  = 1500; /* pause between games (ms)  */
static Uint32 tourney_next_at  = 0;    /* SDL_GetTicks target        */
static int   tourney_waiting   = 0;    /* waiting between games      */
/* Who plays white/black: 0=built-in, 1=UCI1, 2=UCI2, 3=human */
static int   tourney_player[2] = {0, 0};  /* [0]=white, [1]=black   */
static int   tourney_e1_engine = 0;       /* E1's engine type (never swapped) */
/* v14.2: Real round-robin tournament for 3+ engines */
#define MAX_RR_PLAYERS 4
static int   tourney_is_rr = 0;
static int   tourney_rr_num = 0;
static int   tourney_rr_players[MAX_RR_PLAYERS];
static float tourney_rr_score[MAX_RR_PLAYERS];
static int   tourney_rr_sched[24][2];
static int   tourney_rr_sched_len = 0;
static int   tourney_rr_sched_idx = 0;

/* ===================== v15: TOURNAMENT MANAGER (real N-engine round robin) ===================== */
#define MAX_ROSTER 12
typedef struct {
    char   name[48];
    char   path[256];
    int    is_builtin;   /* 1 = built-in StrongEngine, path[] unused        */
    int    slot_hint;    /* last physical uci_eng[] slot loaded into, -1=none */
    int    games, wins, draws, losses;
    double points;
    double sb;           /* Sonneborn-Berger tiebreak                       */
} RosterPlayer;
static RosterPlayer roster[MAX_ROSTER];
static int roster_n = 0;

typedef struct { int a, b; } TMPairing; /* a = white roster idx, b = black roster idx */
#define MAX_TM_PAIRINGS 600
static TMPairing tm_sched[MAX_TM_PAIRINGS];
static int tm_sched_len = 0, tm_sched_idx = 0;
static double tm_crosstable[MAX_ROSTER][MAX_ROSTER];
static int    tm_crosstable_games[MAX_ROSTER][MAX_ROSTER];

static int tm_double_rr          = 1;  /* 1 = each pair also plays with colors reversed */
static int tm_games_per_pairing  = 1;  /* repeat each colored pairing N times           */
static int tm_time_sec           = 300; /* per-game time control for TM games, in seconds */
static int tm_active             = 0;  /* a v15 roster-tournament is currently running   */
static int tm_cur_a = -1, tm_cur_b = -1; /* roster idx of current game's white/black     */

static int tourney_mgr_active = 0; /* is the Tournament Manager panel open?             */
static int tourney_mgr_minimized = 0; /* v17: collapsed to just its title bar, board stays clickable */
static int tm_roster_scroll   = 0;

/* re-uses the existing engine-path text dialog; mode selects what Enter does */
static int path_dialog_mode = 0; /* 0 = load into UCI slot (legacy), 1 = add to roster */

/* ===================== v9: FEN INPUT DIALOG ===================== */
static int   fen_dialog_active = 0;
static char  fen_dialog_buf[256] = "";
static int   fen_dialog_len = 0;

/* v10: engine path input dialog (Linux fallback when no file browser) */
static int   path_dialog_active = 0;
static char  path_dialog_buf[260] = "";
static int   path_dialog_len = 0;
static int   path_dialog_engine_idx = 0;  /* 0=Engine 1, 1=Engine 2 */

/* v14.3: custom tournament games count input */
static int   custom_games_dialog_active = 0;
static char  custom_games_buf[16] = "";
static int   custom_games_len = 0;


/* v10: string option dialog */
static int   stropt_dialog_active = 0;
static int   stropt_is_spin = 0; /* v14: this open dialog edits a Spin option's exact number, not a String */
static char  stropt_dialog_buf[256] = "";
static int   stropt_dialog_len = 0;
static int   stropt_dialog_opt_idx = -1; /* index into uci_eng[...].options */

/* v10: options menu scroll */
static int   options_scroll = 0;
static int   options_engine = 0; /* 0=show engine 1 options, 1=show engine 2 */
/* v12.3: UCI options as a proper overlay window instead of a giant dropdown */
static int   uci_opts_dialog_active = 0;
/* Arrow analysis */
static int arrows[10][4]; /* {fr, fc, tr, tc} */
static int arrow_count = 0;
static int arrow_dragging = 0, arrow_fr, arrow_fc, arrow_mx, arrow_my;

/* ===================== PST TABLES ===================== */
static const int PST_PAWN[64] = {
    0,0,0,0,0,0,0,0, 45,50,50,50,50,50,50,45, 10,15,20,25,25,20,15,10,
    5,5,10,20,20,10,5,5, 0,0,5,10,10,5,0,0, 5,-5,-10,0,0,-10,-5,5,
    5,10,10,0,0,10,10,5, 0,0,0,0,0,0,0,0
};
static const int PST_KNIGHT[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,0,0,0,-20,-40, -30,0,10,15,15,10,0,-30,
    -30,5,15,20,20,15,5,-30, -30,0,15,20,20,15,0,-30, -30,5,10,15,15,10,5,-30,
    -40,-20,0,5,5,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50
};
static const int PST_BISHOP[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,10,10,5,0,-10,
    -10,5,5,10,10,5,5,-10, -10,0,10,10,10,10,0,-10, -10,5,5,5,5,5,5,-10,
    -10,5,0,0,0,0,5,-10, -20,-10,-10,-10,-10,-10,-10,-20
};
static const int PST_ROOK[64] = {
    0,0,0,0,0,0,0,0, 5,10,10,10,10,10,10,5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5,
    -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 0,0,0,5,5,0,0,0
};
static const int PST_QUEEN[64] = {
    -20,-10,-10,-5,-5,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,5,5,5,0,-10,
     -5,0,5,5,5,5,0,-5,  0,0,5,5,5,5,0,0, -10,5,5,5,5,5,0,-10,
    -10,0,5,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20
};
static const int PST_KING_MG[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30, -20,-30,-30,-40,-40,-30,-30,-20, -10,-20,-20,-20,-20,-20,-20,-10,
     20,20,0,0,0,0,20,20,  20,30,10,0,0,10,30,20
};
static const int PST_KING_EG[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50, -30,-20,-10,0,0,-10,-20,-30, -30,-10,20,30,30,20,-10,-30,
    -30,-10,30,40,40,30,-10,-30, -30,-10,30,40,40,30,-10,-30, -30,-10,20,30,30,20,-10,-30,
    -30,-30,0,0,0,0,-30,-30, -50,-30,-30,-30,-30,-30,-30,-50
};
static const int MAT[7]={0,100,320,330,500,900,20000};

/* ===================== LMR TABLE ===================== */
static int lmr_table[64][64];
static void init_lmr_table(void) {
    for(int d=1;d<64;d++) for(int m=1;m<64;m++)
        lmr_table[d][m] = MAX(0, (int)(0.5 + log((double)d) * log((double)m) / 2.0));
}

/* ===================== ZOBRIST ===================== */
static uint64_t zob_rng = 0x123456789ABCDEFULL;
static uint64_t zob_rand64(void) {
    zob_rng ^= zob_rng >> 12; zob_rng ^= zob_rng << 25; zob_rng ^= zob_rng >> 27;
    return zob_rng * 2685821657736338717ULL;
}
static void init_zobrist(void) {
    for(int c=0;c<2;c++) for(int t=0;t<6;t++) for(int s=0;s<64;s++) zobrist_pieces[t*2+c][s]=zob_rand64();
    zobrist_side=zob_rand64();
    for(int i=0;i<8;i++) zobrist_ep[i]=zob_rand64();
    for(int i=0;i<4;i++) zobrist_cr[i]=zob_rand64();
}
static void xor_piece(BBoard *b, int sq, int piece) {
    int col=(piece>0)?WC:BC, typ=abs(piece)-1;
    b->hash ^= zobrist_pieces[typ*2+col][sq];
    if(typ==BB_P) b->pawn_hash ^= zobrist_pieces[typ*2+col][sq];
}
static void xor_cr(BBoard *b, int cr) {
    if(cr&CR_WK) b->hash^=zobrist_cr[0];
    if(cr&CR_WQ) b->hash^=zobrist_cr[1];
    if(cr&CR_BK) b->hash^=zobrist_cr[2];
    if(cr&CR_BQ) b->hash^=zobrist_cr[3];
}
static void xor_side(BBoard *b) { b->hash ^= zobrist_side; }

/* ===================== AUDIO + IMAGES ===================== */
static void beep(int hz,int ms){
    if(!sound_on||!aud)return;
    int n=44100*ms/1000;Sint16*b=malloc(n*2);if(!b)return;
    for(int i=0;i<n;i++){double e=(i<n/5)?(double)i/(n/5):(i>n*4/5)?(double)(n-i)/(n/5):1.0;b[i]=(Sint16)(e*6000*sin(2*M_PI*hz*i/44100));}
    SDL_QueueAudio(aud,b,n*2);free(b);
}
static void load_pieces(void){
    const char*nm[7]={"","p","n","b","r","q","k"};
    const char*cl[2]={"b","w"};
    for(int c=0;c<2;c++)for(int t=1;t<=6;t++){
        char p[128];sprintf(p,"pieces/%s%s.png",cl[c],nm[t]);
        SDL_Surface*s=IMG_Load(p);
        /* v14: pieces are detailed PNG artwork, not the pixel font — Nearest
           neighbor scaling gives jagged/aliased edges whenever SQ_SIZE isn't
           an exact multiple of the source image size. Linear filtering
           smooths curved/diagonal edges (knight, bishop, crown) at any
           square size, which is what makes the pieces look "clear" again. */
        if(s){tex[c][t]=SDL_CreateTextureFromSurface(ren,s);SDL_SetTextureScaleMode(tex[c][t], SDL_ScaleModeLinear);SDL_FreeSurface(s);}
        else tex[c][t]=NULL;
    }
}

/* ===================== ATTACKS + MAGIC ===================== */
static const BB CMK_ROOK_MAGIC[64] = {
    0x8a80104000800020ULL, 0x0140002000100040ULL, 0x2801880a0017001ULL,  0x0100081001000420ULL,
    0x0200020010080420ULL, 0x3001c0002010008ULL,  0x8480008002000100ULL, 0x2080088004402900ULL,
    0x0000800098204000ULL, 0x2024401000200040ULL, 0x0100802000801000ULL, 0x0120800800801000ULL,
    0x0208808088000400ULL, 0x0002802200800400ULL, 0x2200800100020080ULL, 0x0801000060821100ULL,
    0x0080044006422000ULL, 0x0100808020004000ULL, 0x12108a0010204200ULL, 0x0140848010000802ULL,
    0x0481828014002800ULL, 0x8094004002004100ULL, 0x4010040010010802ULL, 0x0000020008806104ULL,
    0x0100400080208000ULL, 0x2040002120081000ULL, 0x0021200680100081ULL, 0x0020100080080080ULL,
    0x0002000a00200410ULL, 0x0000020080800400ULL, 0x0080088400100102ULL, 0x0080004600042881ULL,
    0x4040008040800020ULL, 0x0440003000200801ULL, 0x0004200011004500ULL, 0x0188020010100100ULL,
    0x0014800401802800ULL, 0x2080040080800200ULL, 0x0124080204001001ULL, 0x0200046502000484ULL,
    0x0480400080088020ULL, 0x1000422010034000ULL, 0x0030200100110040ULL, 0x0000100021010009ULL,
    0x2002080100110004ULL, 0x0202008004008002ULL, 0x0020020004010100ULL, 0x2048440040820001ULL,
    0x0101002200408200ULL, 0x0040802000401080ULL, 0x4008142004410100ULL, 0x2060820c0120200ULL,
    0x0001001004080100ULL, 0x020c020080040080ULL, 0x2935610830022400ULL, 0x0044440041009200ULL,
    0x0280001040802101ULL, 0x2100190040002085ULL, 0x80c0084100102001ULL, 0x4024081001000421ULL,
    0x0020030a0244872ULL,  0x0012001008414402ULL, 0x2006104900a0804ULL,  0x0001004081002402ULL,
};
static const BB CMK_BISH_MAGIC[64] = {
    0x0040040844404084ULL, 0x0002004208a04208ULL, 0x0010190041080202ULL, 0x0108060845042010ULL,
    0x0581104180800210ULL, 0x2112080446200010ULL, 0x1080820820060210ULL, 0x03c0808410220200ULL,
    0x0004050404440404ULL, 0x0000021001420088ULL, 0x24d0080801082102ULL, 0x0001020a0a020400ULL,
    0x0000040308200402ULL, 0x0004011002100800ULL, 0x0401484104104005ULL, 0x0801010402020200ULL,
    0x0400210c3880100ULL,  0x0404022024108200ULL, 0x0810018200204102ULL, 0x4002801a02003820ULL,
    0x601c2d0a00610028ULL, 0x0800018200402200ULL, 0x014452a010818000ULL, 0x400a4c0488000401ULL,
    0x0201000182b80404ULL, 0x1000400010002080ULL, 0x4800a0003040080ULL,  0x0100021000a00000ULL,
    0x0010008100208000ULL, 0x0204082002002000ULL, 0x0010008400a04000ULL, 0x4140010000202020ULL,
    0x0140400021002800ULL, 0x00a4804040010400ULL, 0x40040808a0800040ULL, 0x0040004208000410ULL,
    0x0040000840100401ULL, 0x0408020401000080ULL, 0x8000020020041040ULL, 0x0200010028010080ULL,
    0x0104040420088000ULL, 0x1000a010a080820ULL,  0x0208080020008200ULL, 0x0020404040404200ULL,
    0x8001000080800100ULL, 0x0200020900040800ULL, 0x1010c0080200940ULL,  0x0028008000000800ULL,
    0x0040b40880500000ULL, 0x4200200200008200ULL, 0x1041040024080100ULL, 0x0010400040084010ULL,
    0x0200800208004400ULL, 0x0108010008001000ULL, 0x0008020800408020ULL, 0x0080002008408001ULL,
    0x0082000400008002ULL, 0x8010004000010200ULL, 0x0800401010028000ULL, 0x4000020080100081ULL,
    0x0002000a00200410ULL, 0x8040010020004000ULL, 0x4010040810020002ULL, 0x8004000040000801ULL,
};
static BB rook_att_ref(int sq, BB occ) {
    BB r=0; int ro=sq/8, co=sq%8, i;
    for(i=ro-1;i>=0;i--){int s=i*8+co; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    for(i=ro+1;i< 8;i++){int s=i*8+co; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    for(i=co-1;i>=0;i--){int s=ro*8+i; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    for(i=co+1;i< 8;i++){int s=ro*8+i; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    return r;
}
static BB bish_att_ref(int sq, BB occ) {
    BB r=0; int ro=sq/8, co=sq%8, nr, nc;
    for(nr=ro-1,nc=co-1;nr>=0&&nc>=0;nr--,nc--){int s=nr*8+nc; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    for(nr=ro-1,nc=co+1;nr>=0&&nc< 8;nr--,nc++){int s=nr*8+nc; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    for(nr=ro+1,nc=co-1;nr< 8&&nc>=0;nr++,nc--){int s=nr*8+nc; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    for(nr=ro+1,nc=co+1;nr< 8&&nc< 8;nr++,nc++){int s=nr*8+nc; BB_SET(r,s); if(BB_GET(occ,s)) break;}
    return r;
}
static uint64_t mg_rng_state = 0xDEADBEEFCAFEBABEULL;
static uint64_t mg_rand64(void) {
    mg_rng_state ^= mg_rng_state >> 12; mg_rng_state ^= mg_rng_state << 25; mg_rng_state ^= mg_rng_state >> 27;
    return mg_rng_state * 2685821657736338717ULL;
}
static uint64_t mg_sparse(void) { return mg_rand64() & mg_rand64() & mg_rand64(); }
static int try_fill_magic(BB *att_table, int tbl_size, BB mask, int shift, BB magic, BB *oc, BB *at, int cnt) {
    (void)tbl_size; (void)mask;
    /* Generation-counter technique: avoids memset on every call.
       SAFE: only called from init_attacks() at startup, before any threads exist. */
    static int ug[MG_ROOK_TBL]; static BB ua[MG_ROOK_TBL]; static int ge=0; ge++;
    for(int i=0;i<cnt;i++){int idx=(int)((oc[i]*magic)>>(uint32_t)shift);
        if(ug[idx]!=ge){ug[idx]=ge;ua[idx]=at[i];}else if(ua[idx]!=at[i]) return 0;}
    for(int i=0;i<cnt;i++) att_table[(int)((oc[i]*magic)>>(uint32_t)shift)]=at[i];
    return 1;
}
static BB search_magic(BB *att_table, BB mask, int shift, BB *oc, BB *at, int cnt) {
    for(;;){BB magic=mg_sparse(); if(__builtin_popcountll((mask*magic)>>56)<6) continue;
        if(try_fill_magic(att_table,1<<(64-shift),mask,shift,magic,oc,at,cnt)) return magic;}
}
static void init_attacks(void) {
    static const int kn_dr[8]={-2,-2,-1,-1,1,1,2,2},kn_dc[8]={-1,1,-2,2,-2,2,-1,1};
    for(int sq=0;sq<64;sq++){
        int r=sq/8, c=sq%8;
        bb_knight_att[sq]=0;
        for(int i=0;i<8;i++){int nr=r+kn_dr[i],nc=c+kn_dc[i];
            if(nr>=0&&nr<8&&nc>=0&&nc<8) BB_SET(bb_knight_att[sq],nr*8+nc);}
        bb_king_att[sq]=0;
        for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++) if(dr||dc){
            int nr=r+dr,nc=c+dc;
            if(nr>=0&&nr<8&&nc>=0&&nc<8) BB_SET(bb_king_att[sq],nr*8+nc);}
        bb_pawn_att[WC][sq]=0;
        if(r>0){if(c>0) BB_SET(bb_pawn_att[WC][sq],(r-1)*8+c-1);
                if(c<7) BB_SET(bb_pawn_att[WC][sq],(r-1)*8+c+1);}
        bb_pawn_att[BC][sq]=0;
        if(r<7){if(c>0) BB_SET(bb_pawn_att[BC][sq],(r+1)*8+c-1);
                if(c<7) BB_SET(bb_pawn_att[BC][sq],(r+1)*8+c+1);}
        BB rm=0;
        for(int i=r+1;i<7;i++) BB_SET(rm,i*8+c);
        for(int i=r-1;i>0;i--) BB_SET(rm,i*8+c);
        for(int j=c+1;j<7;j++) BB_SET(rm,r*8+j);
        for(int j=c-1;j>0;j--) BB_SET(rm,r*8+j);
        mg_rook_mask[sq]=rm;
        BB bm=0;
        for(int nr=r-1,nc=c-1;nr>0&&nc>0;nr--,nc--) BB_SET(bm,nr*8+nc);
        for(int nr=r-1,nc=c+1;nr>0&&nc<7;nr--,nc++) BB_SET(bm,nr*8+nc);
        for(int nr=r+1,nc=c-1;nr<7&&nc>0;nr++,nc--) BB_SET(bm,nr*8+nc);
        for(int nr=r+1,nc=c+1;nr<7&&nc<7;nr++,nc++) BB_SET(bm,nr*8+nc);
        mg_bish_mask[sq]=bm;
    }
    static BB oc[MG_ROOK_TBL], at[MG_ROOK_TBL];
    for(int sq=0;sq<64;sq++){
        {BB mask=mg_rook_mask[sq]; int bits=__builtin_popcountll(mask), shift=64-bits; mg_rook_shift[sq]=shift;
         int cnt=0; BB sub=0;
         do{oc[cnt]=sub; at[cnt]=rook_att_ref(sq,sub); cnt++; sub=(sub-mask)&mask;}while(sub);
         BB magic=CMK_ROOK_MAGIC[sq];
         if(!try_fill_magic(mg_rook_att[sq],MG_ROOK_TBL,mask,shift,magic,oc,at,cnt))
             magic=search_magic(mg_rook_att[sq],mask,shift,oc,at,cnt);
         mg_rook_magic[sq]=magic;}
        {BB mask=mg_bish_mask[sq]; int bits=__builtin_popcountll(mask), shift=64-bits; mg_bish_shift[sq]=shift;
         int cnt=0; BB sub=0;
         do{oc[cnt]=sub; at[cnt]=bish_att_ref(sq,sub); cnt++; sub=(sub-mask)&mask;}while(sub);
         BB magic=CMK_BISH_MAGIC[sq];
         if(!try_fill_magic(mg_bish_att[sq],MG_BISH_TBL,mask,shift,magic,oc,at,cnt))
             magic=search_magic(mg_bish_att[sq],mask,shift,oc,at,cnt);
         mg_bish_magic[sq]=magic;}
    }
}
static inline BB bb_rook_att(int sq, BB occ){
    BB m=occ & mg_rook_mask[sq];
    return mg_rook_att[sq][(int)((m*mg_rook_magic[sq])>>(uint32_t)mg_rook_shift[sq])];
}
static inline BB bb_bishop_att(int sq, BB occ){
    BB m=occ & mg_bish_mask[sq];
    return mg_bish_att[sq][(int)((m*mg_bish_magic[sq])>>(uint32_t)mg_bish_shift[sq])];
}

/* ===================== BBoard HELPERS ===================== */
static int bb_piece_at(BBoard *b, int col_idx, int sq){
    for(int t=0;t<6;t++) if(BB_GET(b->pieces[col_idx][t],sq)) return t+1;
    return EMPTY;
}
static int bb_piece_at_rc(BBoard *b, int r, int c){
    int sq=r*8+c;
    for(int col=0;col<2;col++) for(int t=0;t<6;t++)
        if(BB_GET(b->pieces[col][t],sq)) return (col==WC?1:-1)*(t+1);
    return EMPTY;
}
static void bb_sync_occ(BBoard *b) {
    b->occ[WC]=b->occ[BC]=0;
    for(int t=0;t<6;t++){b->occ[WC]|=b->pieces[WC][t]; b->occ[BC]|=b->pieces[BC][t];}
    b->occ[2]=b->occ[WC]|b->occ[BC];
}
static int bb_sq_attacked(BBoard *b, int sq, int att) {
    int opp=att^1;
    if(bb_knight_att[sq] & b->pieces[att][BB_N]) return 1;
    if(bb_king_att[sq] & b->pieces[att][BB_K]) return 1;
    if(bb_pawn_att[opp][sq] & b->pieces[att][BB_P]) return 1;
    if(bb_bishop_att(sq,b->occ[2]) & (b->pieces[att][BB_B]|b->pieces[att][BB_Q])) return 1;
    if(bb_rook_att(sq,b->occ[2]) & (b->pieces[att][BB_R]|b->pieces[att][BB_Q])) return 1;
    return 0;
}
static int bb_inchk(BBoard *b, int col_idx){
    if(!b->pieces[col_idx][BB_K]) return 1;
    return bb_sq_attacked(b, BB_LSB(b->pieces[col_idx][BB_K]), col_idx^1);
}

/* ===================== MAKE MOVE (v8.2) ===================== */
static void bb_do(BBoard *b, Move *m, int col_idx) {
    int opp=col_idx^1, f_idx=m->fr*8+m->fc, t_idx=m->tr*8+m->tc;
    int typ=0; for(int t=0;t<6;t++) if(BB_GET(b->pieces[col_idx][t],f_idx)){typ=t;break;}
    int piece_typ=typ+1, moving_piece=(col_idx==WC)?piece_typ:-piece_typ;
    xor_piece(b,f_idx,moving_piece); BB_CLR(b->pieces[col_idx][typ],f_idx);
    if(m->cap){xor_piece(b,t_idx,(opp==WC)?m->cap:-m->cap); BB_CLR(b->pieces[opp][m->cap-1],t_idx);}
    if(m->ep_cap>=0){int ep=m->ep_cap*8+m->tc; xor_piece(b,ep,(opp==WC)?PAWN:-PAWN); BB_CLR(b->pieces[opp][BB_P],ep);}
    if(m->castle==1){xor_piece(b,7*8+7,ROOK);BB_CLR(b->pieces[WC][BB_R],7*8+7);BB_SET(b->pieces[WC][BB_R],7*8+5);xor_piece(b,7*8+5,ROOK);}
    else if(m->castle==2){xor_piece(b,7*8+0,ROOK);BB_CLR(b->pieces[WC][BB_R],7*8+0);BB_SET(b->pieces[WC][BB_R],7*8+3);xor_piece(b,7*8+3,ROOK);}
    else if(m->castle==3){xor_piece(b,0*8+7,-ROOK);BB_CLR(b->pieces[BC][BB_R],0*8+7);BB_SET(b->pieces[BC][BB_R],0*8+5);xor_piece(b,0*8+5,-ROOK);}
    else if(m->castle==4){xor_piece(b,0*8+0,-ROOK);BB_CLR(b->pieces[BC][BB_R],0*8+0);BB_SET(b->pieces[BC][BB_R],0*8+3);xor_piece(b,0*8+3,-ROOK);}
    int place=m->promo?(m->promo-1):typ;
    int new_piece=(col_idx==WC)?(m->promo?m->promo:piece_typ):-(m->promo?m->promo:piece_typ);
    BB_SET(b->pieces[col_idx][place],t_idx); xor_piece(b,t_idx,new_piece); bb_sync_occ(b);
    xor_cr(b,b->cr);
    if(typ==BB_K) b->cr&=~(col_idx==WC?3:12);
    if(f_idx==63||t_idx==63) b->cr&=~1; if(f_idx==56||t_idx==56) b->cr&=~2;
    if(f_idx==7||t_idx==7)   b->cr&=~4; if(f_idx==0||t_idx==0)   b->cr&=~8;
    xor_cr(b,b->cr);
    int old_ep=b->ep; b->ep=-1;
    if(typ==BB_P && abs(m->tr-m->fr)==2) b->ep=m->fc;
    if(old_ep!=-1){b->hash^=zobrist_ep[old_ep]; b->pawn_hash^=zobrist_ep[old_ep];}
    if(b->ep!=-1) {b->hash^=zobrist_ep[b->ep];  b->pawn_hash^=zobrist_ep[b->ep];}
    if(m->cap||m->ep_cap>=0||typ==BB_P) b->fifty=0; else b->fifty++;
    xor_side(b);
}
/* v12: Undo info for make/unmake pattern */
typedef struct {
    int old_cr;
    int old_ep;
    int old_fifty;
    int moved_type;
} UndoInfo;

static inline void bb_save_undo(const BBoard *b, const Move *m, int col_idx, UndoInfo *u) {
    u->old_cr = b->cr;
    u->old_ep = b->ep;
    u->old_fifty = b->fifty;
    int f_idx = m->fr * 8 + m->fc;
    u->moved_type = 0;
    for (int t = 0; t < 6; t++) if (BB_GET(b->pieces[col_idx][t], f_idx)) { u->moved_type = t; break; }
}

static void bb_undo(BBoard *b, const Move *m, int col_idx, const UndoInfo *u) {
    int opp = col_idx ^ 1;
    int f_idx = m->fr * 8 + m->fc;
    int t_idx = m->tr * 8 + m->tc;
    xor_side(b);
    b->fifty = u->old_fifty;
    if (b->ep != -1) { b->hash ^= zobrist_ep[b->ep]; b->pawn_hash ^= zobrist_ep[b->ep]; }
    b->ep = u->old_ep;
    if (u->old_ep != -1) { b->hash ^= zobrist_ep[u->old_ep]; b->pawn_hash ^= zobrist_ep[u->old_ep]; }
    xor_cr(b, b->cr); b->cr = u->old_cr; xor_cr(b, b->cr);
    int place = m->promo ? (m->promo - 1) : u->moved_type;
    int new_piece = (col_idx==WC) ? (m->promo ? m->promo : u->moved_type+1) : -(m->promo ? m->promo : u->moved_type+1);
    xor_piece(b, t_idx, new_piece); BB_CLR(b->pieces[col_idx][place], t_idx);
    if (m->castle==1){xor_piece(b,7*8+5,ROOK);BB_CLR(b->pieces[WC][BB_R],7*8+5);BB_SET(b->pieces[WC][BB_R],7*8+7);xor_piece(b,7*8+7,ROOK);}
    else if(m->castle==2){xor_piece(b,7*8+3,ROOK);BB_CLR(b->pieces[WC][BB_R],7*8+3);BB_SET(b->pieces[WC][BB_R],7*8+0);xor_piece(b,7*8+0,ROOK);}
    else if(m->castle==3){xor_piece(b,0*8+5,-ROOK);BB_CLR(b->pieces[BC][BB_R],0*8+5);BB_SET(b->pieces[BC][BB_R],0*8+7);xor_piece(b,0*8+7,-ROOK);}
    else if(m->castle==4){xor_piece(b,0*8+3,-ROOK);BB_CLR(b->pieces[BC][BB_R],0*8+3);BB_SET(b->pieces[BC][BB_R],0*8+0);xor_piece(b,0*8+0,-ROOK);}
    if (m->cap) {
        int cap_piece = (opp==WC) ? m->cap : -m->cap;
        BB_SET(b->pieces[opp][m->cap-1], t_idx); xor_piece(b, t_idx, cap_piece);
    }
    if (m->ep_cap >= 0) {
        int ep_sq = m->ep_cap * 8 + m->tc;
        int ep_piece = (opp==WC) ? PAWN : -PAWN;
        BB_SET(b->pieces[opp][BB_P], ep_sq); xor_piece(b, ep_sq, ep_piece);
    }
    int orig_piece = (col_idx==WC) ? (u->moved_type+1) : -(u->moved_type+1);
    BB_SET(b->pieces[col_idx][u->moved_type], f_idx); xor_piece(b, f_idx, orig_piece);
    bb_sync_occ(b);
}
/* ===================== MOVE GENERATION (v8.2) ===================== */
static int bb_gen_moves(BBoard *b, int col_idx, Move *mv) {
    bb_sync_occ(b); int n=0, opp=col_idx^1;
    BB my=b->occ[col_idx], en=b->occ[opp], all=b->occ[2];
    BB pawns=b->pieces[col_idx][BB_P];
    while(pawns){
        int sq=BB_LSB(pawns); BB_POP(pawns); int r=sq/8, c=sq%8;
        if(col_idx==WC){
            int pr=r-1; if(pr>=0 && !BB_GET(all,pr*8+c)){
                if(pr==0) for(int pm=QUEEN;pm>=KNIGHT;pm--) mv[n++]=(Move){r,c,pr,c,0,pm,0,-1,0};
                else{mv[n++]=(Move){r,c,pr,c,0,0,0,-1,0}; if(r==6 && !BB_GET(all,(r-2)*8+c)) mv[n++]=(Move){r,c,r-2,c,0,0,0,-1,0};}
            }
            for(int dc=-1;dc<=1;dc+=2){int tr=r-1,tc=c+dc; if(tr<0||tc<0||tc>=8) continue;
                int tsq=tr*8+tc; if(BB_GET(en,tsq)){int cap=bb_piece_at(b,opp,tsq);
                    if(tr==0) for(int pm=QUEEN;pm>=KNIGHT;pm--) mv[n++]=(Move){r,c,tr,tc,cap,pm,0,-1,0};
                    else mv[n++]=(Move){r,c,tr,tc,cap,0,0,-1,0};
                }else if(b->ep>=0 && tc==b->ep && tr==2) mv[n++]=(Move){r,c,tr,tc,0,0,0,r,0};}
        }else{
            int pr=r+1; if(pr<8 && !BB_GET(all,pr*8+c)){
                if(pr==7) for(int pm=QUEEN;pm>=KNIGHT;pm--) mv[n++]=(Move){r,c,pr,c,0,pm,0,-1,0};
                else{mv[n++]=(Move){r,c,pr,c,0,0,0,-1,0}; if(r==1 && !BB_GET(all,(r+2)*8+c)) mv[n++]=(Move){r,c,r+2,c,0,0,0,-1,0};}
            }
            for(int dc=-1;dc<=1;dc+=2){int tr=r+1,tc=c+dc; if(tr>=8||tc<0||tc>=8) continue;
                int tsq=tr*8+tc; if(BB_GET(en,tsq)){int cap=bb_piece_at(b,opp,tsq);
                    if(tr==7) for(int pm=QUEEN;pm>=KNIGHT;pm--) mv[n++]=(Move){r,c,tr,tc,cap,pm,0,-1,0};
                    else mv[n++]=(Move){r,c,tr,tc,cap,0,0,-1,0};
                }else if(b->ep>=0 && tc==b->ep && tr==5) mv[n++]=(Move){r,c,tr,tc,0,0,0,r,0};}
        }
    }
    for(int typ=BB_N;typ<=BB_K;typ++){
        BB pp=b->pieces[col_idx][typ]; while(pp){
            int sq=BB_LSB(pp); BB_POP(pp); int r=sq/8,c=sq%8;
            BB att=(typ==BB_N)?bb_knight_att[sq]:(typ==BB_B)?bb_bishop_att(sq,all):
                   (typ==BB_R)?bb_rook_att(sq,all):(typ==BB_Q)?(bb_bishop_att(sq,all)|bb_rook_att(sq,all)):bb_king_att[sq];
            att&=~my; while(att){int tsq=BB_LSB(att);BB_POP(att);
                mv[n++]=(Move){r,c,tsq/8,tsq%8,bb_piece_at(b,opp,tsq),0,0,-1,0};}
            if(typ==BB_K){
                if(r==7&&c==4 && col_idx==WC){
                    if((b->cr&CR_WK)&&!BB_GET(all,61)&&!BB_GET(all,62)&&BB_GET(b->pieces[WC][BB_R],63)&&
                       !bb_sq_attacked(b,60,BC)&&!bb_sq_attacked(b,61,BC)&&!bb_sq_attacked(b,62,BC))
                        mv[n++]=(Move){7,4,7,6,0,0,1,-1,0};
                    if((b->cr&CR_WQ)&&!BB_GET(all,59)&&!BB_GET(all,58)&&!BB_GET(all,57)&&BB_GET(b->pieces[WC][BB_R],56)&&
                       !bb_sq_attacked(b,60,BC)&&!bb_sq_attacked(b,59,BC)&&!bb_sq_attacked(b,58,BC))
                        mv[n++]=(Move){7,4,7,2,0,0,2,-1,0};
                }
                if(r==0&&c==4 && col_idx==BC){
                    if((b->cr&CR_BK)&&!BB_GET(all,5)&&!BB_GET(all,6)&&BB_GET(b->pieces[BC][BB_R],7)&&
                       !bb_sq_attacked(b,4,WC)&&!bb_sq_attacked(b,5,WC)&&!bb_sq_attacked(b,6,WC))
                        mv[n++]=(Move){0,4,0,6,0,0,3,-1,0};
                    if((b->cr&CR_BQ)&&!BB_GET(all,3)&&!BB_GET(all,2)&&!BB_GET(all,1)&&BB_GET(b->pieces[BC][BB_R],0)&&
                       !bb_sq_attacked(b,4,WC)&&!bb_sq_attacked(b,3,WC)&&!bb_sq_attacked(b,2,WC))
                        mv[n++]=(Move){0,4,0,2,0,0,4,-1,0};
                }
            }
        }
    }
    return n;
}


/* ===================== OUTPOST ===================== */
static int is_outpost(BBoard *b, int sq, int col) {
    int r=sq/8, c=sq%8;
    int min_r=(col==WC)?3:2, max_r=(col==WC)?6:5;
    if(r<min_r||r>max_r) return 0;
    if(col==WC){
        if(c>0 && r>0 && BB_GET(b->pieces[WC][BB_P],(r-1)*8+c-1)) return 1;
        if(c<7 && r>0 && BB_GET(b->pieces[WC][BB_P],(r-1)*8+c+1)) return 1;
    } else {
        if(c>0 && r<7 && BB_GET(b->pieces[BC][BB_P],(r+1)*8+c-1)) return 1;
        if(c<7 && r<7 && BB_GET(b->pieces[BC][BB_P],(r+1)*8+c+1)) return 1;
    }
    return 0;
}

/* ===================== PST + EVAL ===================== */
static int get_pst_score(int piece, int square, int endgame) {
    int r=square/8, c=square%8, idx=(piece>0)?r:7-r;
    switch(abs(piece)){
        case PAWN:return PST_PAWN[idx*8+c]; case KNIGHT:return PST_KNIGHT[idx*8+c];
        case BISHOP:return PST_BISHOP[idx*8+c]; case ROOK:return PST_ROOK[idx*8+c];
        case QUEEN:return PST_QUEEN[idx*8+c];
        case KING:return endgame?PST_KING_EG[idx*8+c]:PST_KING_MG[idx*8+c];
    } return 0;
}

/* Cached pawn structure evaluation */
static int eval_pawns(BBoard *b, BB passed_out[2]) {
    PawnTTEntry *pte = &pawn_tt[b->pawn_hash & PAWN_TT_MASK];
    if(pte->key == b->pawn_hash) {
        if(passed_out) { passed_out[0] = pte->passed[0]; passed_out[1] = pte->passed[1]; }
        return pte->score;
    }
    int score = 0;
    static const int sgn[2] = {1, -1};
    BB passed[2] = {0, 0};
    for(int col=0; col<2; col++) {
        int sign = sgn[col];
        BB pawns = b->pieces[col][BB_P], opp = b->pieces[col^1][BB_P];
        for(int f=0; f<8; f++) {
            BB fm=0; for(int r=0; r<8; r++) BB_SET(fm, r*8+f);
            int cnt2 = __builtin_popcountll(pawns & fm);
            if(cnt2 > 1) score += sign * (-25 * (cnt2-1));
        }
        for(int f=0; f<8; f++) {
            BB fm=0; for(int r=0; r<8; r++) BB_SET(fm, r*8+f);
            if(!(pawns & fm)) continue;
            BB adj=0;
            if(f>0) for(int r=0; r<8; r++) BB_SET(adj, r*8+f-1);
            if(f<7) for(int r=0; r<8; r++) BB_SET(adj, r*8+f+1);
            if(!(pawns & adj)) {
                int eof = !!(opp & fm);
                score += sign * (eof ? -20 : (f==0||f==7 ? -25 : -30));
            }
        }
        BB tmp = pawns;
        while(tmp) {
            int sq = BB_LSB(tmp); BB_POP(tmp); int r = sq/8;
            int sr=(col==WC)?6:1, os=(col==WC)?5:2;
            if(r!=sr && r!=os) continue;
            int bs = (col==WC) ? sq-8 : sq+8;
            if(bs>=0 && bs<64) {
                BB blk = b->occ[col] & ~pawns & ~b->pieces[col][BB_K];
                if(BB_GET(blk, bs)) score += sign * (-20);
            }
        }
        tmp = pawns;
        while(tmp) {
            int sq = BB_LSB(tmp); BB_POP(tmp); int r = sq/8, f = sq%8;
            BB cone = 0;
            if(col==WC) { for(int nr=r-1; nr>=0; nr--) { BB_SET(cone, nr*8+f); if(f>0) BB_SET(cone, nr*8+f-1); if(f<7) BB_SET(cone, nr*8+f+1); } }
            else { for(int nr=r+1; nr<8; nr++) { BB_SET(cone, nr*8+f); if(f>0) BB_SET(cone, nr*8+f-1); if(f<7) BB_SET(cone, nr*8+f+1); } }
            if(!(opp & cone)) {
                BB_SET(passed[col], sq);
                int rk2 = (col==WC) ? 7-r : r;
                int pp_bonus = 25 + rk2*rk2*12;
                int connected = 0;
                for(int df=-1; df<=1; df+=2) {
                    int af = f+df; if(af<0 || af>=8) continue;
                    BB adj_file=0; for(int r2=0; r2<8; r2++) BB_SET(adj_file, r2*8+af);
                    BB adj_pawns = pawns & adj_file;
                    if(adj_pawns) {
                        BB adj_tmp = adj_pawns;
                        while(adj_tmp) { int as = BB_LSB(adj_tmp); BB_POP(adj_tmp);
                            int ar = as/8; if(abs(ar-r)<=1) { connected=1; break; }
                        }
                    }
                    if(connected) break;
                }
                if(connected) pp_bonus += 20;
                score += sign * pp_bonus;
            }
        }
    }
    pte->key = b->pawn_hash; pte->score = score;
    pte->passed[0] = passed[0]; pte->passed[1] = passed[1];
    if(passed_out) { passed_out[0] = passed[0]; passed_out[1] = passed[1]; }
    return score;
}

static int bb_eval(BBoard *b, int col_idx) {
    int mg=0,eg=0,phase=0; static const int sgn[2]={1,-1};
    for(int col=0;col<2;col++){
        phase+=__builtin_popcountll(b->pieces[col][BB_N])*1;
        phase+=__builtin_popcountll(b->pieces[col][BB_B])*1;
        phase+=__builtin_popcountll(b->pieces[col][BB_R])*2;
        phase+=__builtin_popcountll(b->pieces[col][BB_Q])*4;
    }
    phase=(phase*256)/24; if(phase>256)phase=256;
    for(int col=0;col<2;col++){for(int typ=0;typ<6;typ++){BB bb2=b->pieces[col][typ];while(bb2){
        int sq=BB_LSB(bb2);BB_POP(bb2); int p=sgn[col]*(typ+1);
        if(typ!=BB_K){mg+=sgn[col]*(MAT[typ+1]+get_pst_score(p,sq,0)); eg+=sgn[col]*(MAT[typ+1]+get_pst_score(p,sq,1));}
        else{mg+=sgn[col]*get_pst_score(p,sq,0); eg+=sgn[col]*get_pst_score(p,sq,1);}}}}
    int score=(mg*phase+eg*(256-phase))/256;
    BB all=b->occ[2];
    for(int col=0;col<2;col++){
        int mob_kn=0,mob_bi=0,mob_rk=0,mob_qn=0,sign=sgn[col];
        BB opp_patt=0;
        {BB pp=b->pieces[col^1][BB_P];while(pp){int s=BB_LSB(pp);BB_POP(pp);opp_patt|=bb_pawn_att[col^1][s];}}
        BB safe=~b->occ[col]&~opp_patt;
        BB kn=b->pieces[col][BB_N];while(kn){int s=BB_LSB(kn);BB_POP(kn);mob_kn+=__builtin_popcountll(bb_knight_att[s]&safe);}
        BB bi=b->pieces[col][BB_B];while(bi){int s=BB_LSB(bi);BB_POP(bi);mob_bi+=__builtin_popcountll(bb_bishop_att(s,all)&safe);}
        BB rk=b->pieces[col][BB_R];while(rk){int s=BB_LSB(rk);BB_POP(rk);mob_rk+=__builtin_popcountll(bb_rook_att(s,all)&~b->occ[col]);}
        BB qn=b->pieces[col][BB_Q];while(qn){int s=BB_LSB(qn);BB_POP(qn);mob_qn+=__builtin_popcountll((bb_bishop_att(s,all)|bb_rook_att(s,all))&~b->occ[col]);}
        score+=sign*(mob_kn*4 + mob_bi*4 + mob_rk*3 + mob_qn*1);
    }
    int opening = (phase > 128) ? (phase - 128) : 0;
    int dev_scale = opening / 32;
    for(int col=0; col<2; col++){
        int sign=sgn[col];
        int back=(col==WC)?7:0;
        BB mask = ~(((BB)0xFF) << (back*8));
        BB dev = (b->pieces[col][BB_N] | b->pieces[col][BB_B]) & mask;
        score += sign * (__builtin_popcountll(dev) * (15 + dev_scale*5));
    }
    {int centers[4]={27,28,35,36};
    for(int col=0;col<2;col++){int sign=sgn[col];BB pw=b->pieces[col][BB_P];
        for(int i=0;i<4;i++) if(BB_GET(pw,centers[i])) score+=sign*25;}}
    for(int col=0;col<2;col++){
        int sign=sgn[col];
        int d_start=(col==WC)?51:11, e_start=(col==WC)?52:12;
        int d_block=(col==WC)?d_start-8:d_start+8, e_block=(col==WC)?e_start-8:e_start+8;
        if(BB_GET(b->pieces[col][BB_P],d_start)&&BB_GET(b->pieces[col][BB_B]|b->pieces[col][BB_N],d_block))
            score+=sign*(-60-dev_scale*10);
        if(BB_GET(b->pieces[col][BB_P],e_start)&&BB_GET(b->pieces[col][BB_B]|b->pieces[col][BB_N],e_block))
            score+=sign*(-60-dev_scale*10);
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col], sq1=(col==WC)?45:21, sq2=(col==WC)?42:18;
        BB kn=b->pieces[col][BB_N];
        if(BB_GET(kn,sq1)) score+=sign*30; if(BB_GET(kn,sq2)) score+=sign*30;
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col];
        if(__builtin_popcountll(b->pieces[col][BB_B])>=2)
            score+=sign*(50+(256-phase)/8);
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col]; BB rks=b->pieces[col][BB_R];
        while(rks){int s=BB_LSB(rks);BB_POP(rks); int f=s%8;
            BB fm=0; for(int r=0;r<8;r++) BB_SET(fm,r*8+f);
            int op=!!(b->pieces[col][BB_P]&fm), ep=!!(b->pieces[col^1][BB_P]&fm);
            if(!op&&!ep) score+=sign*35; else if(!op) score+=sign*20;}
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col], rank7=(col==WC)?1:6;
        BB rks=b->pieces[col][BB_R];
        while(rks){int s=BB_LSB(rks);BB_POP(rks); if(s/8==rank7) score+=sign*25;}
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col];
        BB kn=b->pieces[col][BB_N]; while(kn){int s=BB_LSB(kn);BB_POP(kn);
            if(is_outpost(b,s,col)) score+=sign*25;}
        BB bi=b->pieces[col][BB_B]; while(bi){int s=BB_LSB(bi);BB_POP(bi);
            if(is_outpost(b,s,col)) score+=sign*18;}
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col];
        int back_rank=(col==WC)?7:0; int pawn_rank=(col==WC)?6:1;
        BB bi=b->pieces[col][BB_B];
        while(bi){
            int s=BB_LSB(bi);BB_POP(bi);
            int r=s/8,c=s%8;
            if((r==back_rank||r==pawn_rank)&&(c==0||c==7)){
                int block_sq=(col==WC)?s-8:s+8;
                if(block_sq>=0&&block_sq<64&&BB_GET(b->pieces[col][BB_P],block_sq))
                    score+=sign*(-80);
            }
        }
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col]; BB ek=b->pieces[col^1][BB_K]; if(!ek) continue;
        int ek_sq=BB_LSB(ek); int ek_r=ek_sq/8,ek_c=ek_sq%8;
        BB rq=b->pieces[col][BB_R]|b->pieces[col][BB_Q];
        while(rq){
            int s=BB_LSB(rq);BB_POP(rq);
            int sr2=s/8,sc2=s%8;
            if(sr2==ek_r||sc2==ek_c){
                BB ray=0;
                if(sr2==ek_r){for(int f2=MIN(sc2,ek_c)+1;f2<MAX(sc2,ek_c);f2++)BB_SET(ray,sr2*8+f2);}
                else{for(int r2=MIN(sr2,ek_r)+1;r2<MAX(sr2,ek_r);r2++)BB_SET(ray,r2*8+sc2);}
                BB ray_occ=ray&b->occ[2];
                int pop=__builtin_popcountll(ray_occ);
                if(pop==1&&(ray_occ&b->occ[col^1]))score+=sign*25;
            }
        }
        BB bq=b->pieces[col][BB_B]|b->pieces[col][BB_Q];
        while(bq){
            int s=BB_LSB(bq);BB_POP(bq);
            int sr2=s/8,sc2=s%8;
            int dr=ek_r-sr2,dc=ek_c-sc2;
            if(dr!=0&&abs(dr)==abs(dc)){
                int sdr=(dr>0)?1:-1; int sdc=(dc>0)?1:-1;
                BB ray=0; int cr2=sr2+sdr,cc2=sc2+sdc;
                while(cr2!=ek_r||cc2!=ek_c){
                    if(cr2>=0&&cr2<8&&cc2>=0&&cc2<8)BB_SET(ray,cr2*8+cc2);
                    cr2+=sdr;cc2+=sdc;
                }
                BB ray_occ=ray&b->occ[2];
                int pop=__builtin_popcountll(ray_occ);
                if(pop==1&&(ray_occ&b->occ[col^1]))score+=sign*25;
            }
        }
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col]; BB rks=b->pieces[col][BB_R];
        while(rks){
            int s=BB_LSB(rks);BB_POP(rks); int r=s/8,f=s%8;
            BB qn=b->pieces[col][BB_Q];
            while(qn){
                int qs=BB_LSB(qn);BB_POP(qn);
                if(qs%8==f){
                    int r1=MIN(r,qs/8),r2=MAX(r,qs/8);
                    BB file_between=0; for(int rr=r1+1;rr<r2;rr++)BB_SET(file_between,rr*8+f);
                    if(!(file_between&b->occ[col]))score+=sign*20;
                }
            }
            BB pawns=b->pieces[col][BB_P];
            while(pawns){
                int ps=BB_LSB(pawns);BB_POP(pawns);
                if(ps%8!=f)continue;
                int pr=ps/8;
                if((col==WC&&r<pr)||(col==BC&&r>pr)){
                    int r1=MIN(r,pr),r2=MAX(r,pr);
                    BB file_between=0; for(int rr=r1+1;rr<r2;rr++)BB_SET(file_between,rr*8+f);
                    if(!(file_between&b->occ[col]))score+=sign*15;
                }
            }
        }
    }
    for(int col=0;col<2;col++){
        int sign=sgn[col]; BB ek=b->pieces[col^1][BB_K]; if(!ek) continue;
        int ek_sq=BB_LSB(ek); int ek_r=ek_sq/8,ek_c=ek_sq%8;
        int tropism=0;
        {BB kn=b->pieces[col][BB_N];while(kn){int s=BB_LSB(kn);BB_POP(kn);
            tropism+=MAX(0,(5-MAX(abs(s/8-ek_r),abs(s%8-ek_c)))*3);}}
        {BB bi=b->pieces[col][BB_B];while(bi){int s=BB_LSB(bi);BB_POP(bi);
            tropism+=MAX(0,(5-MAX(abs(s/8-ek_r),abs(s%8-ek_c)))*2);}}
        {BB rk=b->pieces[col][BB_R];while(rk){int s=BB_LSB(rk);BB_POP(rk);
            if(s/8==ek_r||s%8==ek_c)tropism+=5;}}
        {BB qn=b->pieces[col][BB_Q];while(qn){int s=BB_LSB(qn);BB_POP(qn);
            tropism+=MAX(0,(5-MAX(abs(s/8-ek_r),abs(s%8-ek_c)))*2);
            if(s/8==ek_r||s%8==ek_c)tropism+=3;}}
        if(phase>60)score+=sign*tropism;
    }
    if(phase>100){
        for(int col=0;col<2;col++){
            int sign=sgn[col]; BB center=0;
            if(col==WC){for(int sq=24;sq<48;sq++)BB_SET(center,sq);}
            else{for(int sq=16;sq<40;sq++)BB_SET(center,sq);}
            BB ctrl=0;
            {BB kn=b->pieces[col][BB_N];while(kn){int s=BB_LSB(kn);BB_POP(kn);ctrl|=bb_knight_att[s]&center;}}
            {BB bi=b->pieces[col][BB_B];while(bi){int s=BB_LSB(bi);BB_POP(bi);ctrl|=bb_bishop_att(s,all)&center;}}
            {BB rk=b->pieces[col][BB_R];while(rk){int s=BB_LSB(rk);BB_POP(rk);ctrl|=bb_rook_att(s,all)&center;}}
            {BB pw=b->pieces[col][BB_P];while(pw){int s=BB_LSB(pw);BB_POP(pw);ctrl|=bb_pawn_att[col][s]&center;}}
            score+=sign*(__builtin_popcountll(ctrl)*2);
        }
    }
    {
        BB passed[2] = {0, 0};
        score += eval_pawns(b, passed);
        if(phase < 128) {
            for(int col=0; col<2; col++) {
                int sign = sgn[col]; BB pp = passed[col];
                while(pp) {
                    int sq = BB_LSB(pp); BB_POP(pp);
                    int r = sq/8, f = sq%8;
                    BB kg = b->pieces[col][BB_K];
                    if(kg) { int ks=BB_LSB(kg), kr2=ks/8, kf2=ks%8;
                        int dist = MAX(abs(kr2-r), abs(kf2-f));
                        score += sign * MAX(0, (7-dist)*5);
                    }
                    BB okg = b->pieces[col^1][BB_K];
                    if(okg) { int oks=BB_LSB(okg), okr=oks/8, okf=oks%8;
                        int dist = MAX(abs(okr-r), abs(okf-f));
                        score -= sign * MAX(0, (7-dist)*3);
                    }
                }
            }
        }
    }
    if(phase>40){
        for(int col=0;col<2;col++){
            int sign=sgn[col]; BB ekg=b->pieces[col^1][BB_K];if(!ekg)continue;
            int ksq=BB_LSB(ekg),kr2=ksq/8,kf=ksq%8;
            BB kz=bb_king_att[ksq]|(1ULL<<ksq);
            int att_weight=0, att_count=0;
            BB kn2=b->pieces[col][BB_N];while(kn2){int ns=BB_LSB(kn2);BB_POP(kn2);if(bb_knight_att[ns]&kz){att_weight+=3;att_count++;}}
            BB bi2=b->pieces[col][BB_B];while(bi2){int ns=BB_LSB(bi2);BB_POP(bi2);if(bb_bishop_att(ns,all)&kz){att_weight+=2;att_count++;}}
            BB rk2=b->pieces[col][BB_R];while(rk2){int ns=BB_LSB(rk2);BB_POP(rk2);if(bb_rook_att(ns,all)&kz){att_weight+=2;att_count++;}}
            BB qn2=b->pieces[col][BB_Q];while(qn2){int ns=BB_LSB(qn2);BB_POP(qn2);
                int qatt=0;if(bb_bishop_att(ns,all)&kz)qatt++;if(bb_rook_att(ns,all)&kz)qatt++;
                if(qatt){att_weight+=qatt*4;att_count+=qatt;}}
            static const int kdt[9]={0,15,40,80,130,190,260,340,440};
            int danger=(att_count>0)?kdt[att_count<8?att_count:8]:0;
            danger = danger * (100 + att_weight * 8) / 100;
            {BB qn=b->pieces[col][BB_Q];while(qn){int qs=BB_LSB(qn);BB_POP(qn);
                int qr=qs/8,qf=qs%8;int qdist=MAX(abs(qr-kr2),abs(qf-kf));
                if(qdist<=3) danger+=(4-qdist)*15;}}
            for(int df=-1;df<=1;df++){int ff=kf+df;if(ff<0||ff>=8)continue;
                BB fm=0;for(int r=0;r<8;r++)BB_SET(fm,r*8+ff);
                int op=!!(b->pieces[col][BB_P]&fm), ep=!!(b->pieces[col^1][BB_P]&fm);
                if(!op&&!ep) danger+=15; else if(!op) danger+=8;}
            score+=sign*danger;
        }
        for(int col=0;col<2;col++){int sign=sgn[col]; BB kg=b->pieces[col][BB_K];if(!kg)continue;
            int ksq=BB_LSB(kg),kr=ksq/8,kf=ksq%8;
            int sh=0,pr=(col==WC)?kr-1:kr+1;
            if(pr>=0&&pr<8)for(int f=kf-1;f<=kf+1;f++)if(f>=0&&f<8&&BB_GET(b->pieces[col][BB_P],pr*8+f))sh++;
            score+=sign*(sh*18-25);}
    }
    /* v12.5: King mobility in endgame — encourages the king to become active */
    if(phase < 80) {
        for(int col=0; col<2; col++) {
            int sign = sgn[col];
            BB kg = b->pieces[col][BB_K];
            if(!kg) continue;
            int ksq = BB_LSB(kg);
            BB mob = bb_king_att[ksq] & ~b->occ[col] & ~(b->pieces[col^1][BB_P]);
            /* penalize king on edges/corners in endgame */
            int kr = ksq/8, kf = ksq%8;
            int center_dist = MAX(abs(kr*2 - 7), abs(kf*2 - 7)); /* 0..7 mapped to distance from center */
            score += sign * (12 - center_dist * 2); /* centralize king */
            score += sign * (__builtin_popcountll(mob) * 4);
        }
    }
    /* v12.5: Passed pawn race evaluation — in endgame, bonus for advanced passers
       that are closer to promotion and whose own king supports the push */
    {
        BB passed[2] = {0, 0};
        /* Reuse the passed pawn computation already done in eval_pawns via pawn TT.
           For simplicity, compute a quick version here for the race bonus. */
        for(int col=0; col<2; col++) {
            BB pawns = b->pieces[col][BB_P];
            BB opp_pawns = b->pieces[col^1][BB_P];
            while(pawns) {
                int sq = BB_LSB(pawns); BB_POP(pawns);
                int r = sq/8, f = sq%8;
                BB cone = 0;
                if(col == WC) {
                    for(int nr = r-1; nr >= 0; nr--) {
                        BB_SET(cone, nr*8+f);
                        if(f > 0) BB_SET(cone, nr*8+f-1);
                        if(f < 7) BB_SET(cone, nr*8+f+1);
                    }
                } else {
                    for(int nr = r+1; nr < 8; nr++) {
                        BB_SET(cone, nr*8+f);
                        if(f > 0) BB_SET(cone, nr*8+f-1);
                        if(f < 7) BB_SET(cone, nr*8+f+1);
                    }
                }
                if(!(opp_pawns & cone)) BB_SET(passed[col], sq);
            }
        }
        if(phase < 140) {
            for(int col=0; col<2; col++) {
                int sign = sgn[col];
                BB pp = passed[col];
                while(pp) {
                    int sq = BB_LSB(pp); BB_POP(pp);
                    int r = sq/8, f = sq%8;
                    int promo_rank = (col == WC) ? 7 : 0;
                    int dist_to_promo = abs(r - promo_rank);
                    /* bonus scales quadratically with advancement */
                    int race_bonus = (7 - dist_to_promo) * (7 - dist_to_promo) * 3;
                    /* bonus if own king is nearby to support the push */
                    BB own_king = b->pieces[col][BB_K];
                    if(own_king) {
                        int ksq = BB_LSB(own_king);
                        int k_dist = MAX(abs(ksq/8 - r), abs(ksq%8 - f));
                        race_bonus += (6 - MIN(k_dist, 6)) * 4;
                    }
                    /* penalty if enemy king is closer to the promotion square */
                    BB opp_king = b->pieces[col^1][BB_K];
                    if(opp_king) {
                        int oksq = BB_LSB(opp_king);
                        int ok_dist = MAX(abs(oksq/8 - promo_rank), abs(oksq%8 - f));
                        race_bonus -= (6 - MIN(ok_dist, 6)) * 3;
                    }
                    score += sign * race_bonus;
                }
            }
        }
    }
    /* v8.3: Tempo bonus — small bonus for side to move */
    score += (col_idx == WC ? 15 : -15);
    return score;
}


/* ===================== REPETITION ===================== */
static int is_repetition(uint64_t hash, int ply) {
    int cnt=0;
    for(int i=0;i<game_hist_n;i++) if(game_hist_hashes[i]==hash) cnt++;
    for(int i=0;i<ply;i++) if(line_hashes[i]==hash) cnt++;
    return cnt>=2;
}

/* -- v8.3.1: Insufficient Material Detection -- */
static int is_insufficient_material(BBoard *b) {
    BB all = b->occ[WC] | b->occ[BC];
    /* K vs K */
    if(all == (b->pieces[WC][BB_K] | b->pieces[BC][BB_K])) return 1;
    /* K+B vs K or K+N vs K (either side) */
    for(int col=0; col<2; col++) {
        int opp = col ^ 1;
        BB minor = b->pieces[col][BB_B] | b->pieces[col][BB_N];
        if(minor && !(minor & (minor - 1))) {
            if(b->occ[opp] == b->pieces[opp][BB_K]) return 1;
        }
    }
    /* K+B vs K+B same color squares = draw */
    {
        BB wb = b->pieces[WC][BB_B], bb = b->pieces[BC][BB_B];
        if(wb && bb && !(wb & (wb-1)) && !(bb & (bb-1))) {
            if(b->occ[WC]==(b->pieces[WC][BB_K]|wb) && b->occ[BC]==(b->pieces[BC][BB_K]|bb)) {
                int wsq=BB_LSB(wb), bsq=BB_LSB(bb);
                if(((wsq/8+wsq%8)&1) == ((bsq/8+bsq%8)&1)) return 1;
            }
        }
    }
    /* K+N+N vs K (either side) = draw */
    for(int col=0; col<2; col++) {
        int opp = col ^ 1;
        BB knights = b->pieces[col][BB_N];
        int ncnt = __builtin_popcountll(knights);
        if(ncnt == 2 && b->occ[col] == (b->pieces[col][BB_K] | knights)
           && b->occ[opp] == b->pieces[opp][BB_K]) return 1;
    }
    return 0;
}

/* ===================== SEE & SCORING ===================== */
static int lva_sq(BBoard *b,int to_sq,int stm,BB occ,BB *from_bb){
    BB att; att=bb_pawn_att[stm^1][to_sq]&b->pieces[stm][BB_P]&occ;if(att){*from_bb=att&-(int64_t)att;return MAT[PAWN];}
    att=bb_knight_att[to_sq]&b->pieces[stm][BB_N]&occ;if(att){*from_bb=att&-(int64_t)att;return MAT[KNIGHT];}
    att=bb_bishop_att(to_sq,occ)&b->pieces[stm][BB_B]&occ;if(att){*from_bb=att&-(int64_t)att;return MAT[BISHOP];}
    att=bb_rook_att(to_sq,occ)&b->pieces[stm][BB_R]&occ;if(att){*from_bb=att&-(int64_t)att;return MAT[ROOK];}
    att=(bb_bishop_att(to_sq,occ)|bb_rook_att(to_sq,occ))&b->pieces[stm][BB_Q]&occ;if(att){*from_bb=att&-(int64_t)att;return MAT[QUEEN];}
    att=bb_king_att[to_sq]&b->pieces[stm][BB_K]&occ;if(att){*from_bb=att&-(int64_t)att;return MAT[KING];}
    *from_bb=0;return 0;
}
static int see_rec(BBoard *b,int to_sq,int stm,BB occ,int av){BB fb;int pv=lva_sq(b,to_sq,stm,occ,&fb);if(!fb)return 0;int v=av-see_rec(b,to_sq,stm^1,occ&~fb,pv);return v>0?v:0;}
static int bb_see(BBoard *b,Move *m,int stm){
    int fs=m->fr*8+m->fc,ts=m->tr*8+m->tc; int cv=m->cap?MAT[m->cap]:(m->ep_cap>=0?MAT[PAWN]:0);int pv=0;
    for(int t=0;t<6;t++)if(BB_GET(b->pieces[stm][t],fs)){pv=MAT[t+1];break;}
    return cv-see_rec(b,ts,stm^1,b->occ[2]&~(1ULL<<fs),pv);
}
static int mvv_lva(int v,int a){return (v<<3)+(7-a);}

static void score_moves(Move *mv,int cnt,int col,int ply,const Move *par,BBoard *b){
    int si=(col==WC)?0:1;
    for(int i=0;i<cnt;i++){
        int sc=0;
        if(mv[i].cap||mv[i].ep_cap>=0){
            /* v8.3: Use Capture History + MVV-LVA for improved capture ordering */
            int piece=bb_piece_at(b,col,mv[i].fr*8+mv[i].fc);
            int p_idx=(col==WC)?piece-1:piece+5;
            int sv=bb_see(b,&mv[i],col);
            sc=10000+mvv_lva(mv[i].cap?mv[i].cap:PAWN,piece)+cap_history[p_idx][mv[i].tr*8+mv[i].tc]/100;
            if(sv<0) sc=sv; /* still penalize SEE-losing captures */
        } else if(mv[i].promo){
            sc=30000+MAT[mv[i].promo];
        } else {
            for(int k=0;k<2&&k<killer_cnt[ply];k++)
                if(killer[ply][k].fr==mv[i].fr&&killer[ply][k].fc==mv[i].fc&&
                   killer[ply][k].tr==mv[i].tr&&killer[ply][k].tc==mv[i].tc){
                    sc=8000-k*100; break;
                }
            if(sc==0&&par&&par->fr>=0){
                Move cm=counter[si][par->fr*8+par->fc][par->tr*8+par->tc];
                if(cm.fr==mv[i].fr&&cm.fc==mv[i].fc&&cm.tr==mv[i].tr&&cm.tc==mv[i].tc) sc=6000;
                else {
                    int opp_cap_piece=bb_piece_at_rc(b,par->tr,par->tc);
                    if(opp_cap_piece!=EMPTY){
                        int opp_abs=abs(opp_cap_piece);
                        if(opp_abs>=PAWN&&opp_abs<=QUEEN){
                            int opp_pci=(opp_cap_piece>0)?opp_abs-1:opp_abs+5;
                            sc+=cmh[opp_pci][par->tr*8+par->tc]/2;
                        }
                    }
                }
            }
            if(sc==0){
                sc=history[si][mv[i].fr*8+mv[i].fc][mv[i].tr*8+mv[i].tc];
                if(par&&par->fr>=0){
                    int cpt=bb_piece_at(b,col,mv[i].fr*8+mv[i].fc);
                    if(cpt>=PAWN&&cpt<=QUEEN){
                        int cpci=(col==WC)?cpt-1:cpt+5;
                        int ppt=bb_piece_at(b,col^1,par->tr*8+par->tc);
                        if(ppt>=PAWN&&ppt<=QUEEN){
                            int ppci=((col^1)==WC)?ppt-1:ppt+5;
                            sc+=cont_hist[ppci][cpci][mv[i].tr*8+mv[i].tc]/4;
                        }
                    }
                }
            }
        }
        mv[i].score=sc;
    }
    /* Partial selection sort: bubble the highest score to position 0, then
       use insertion sort only for the remaining.  In practice the TT move is
       already at index 0 (score 1000000), so we mostly sort the captures/killers
       which are already near the front.  This avoids the O(n²) worst case of
       pure insertion sort on ~200+ move lists (e.g. in qsearch positions). */
    if(cnt > 1) {
        /* find the maximum and swap to front */
        int best_idx = 0;
        for(int i = 1; i < cnt; i++) {
            if(mv[i].score > mv[best_idx].score) best_idx = i;
        }
        if(best_idx != 0) { Move tmp = mv[0]; mv[0] = mv[best_idx]; mv[best_idx] = tmp; }
        /* insertion sort for the rest — typically small after TT/capture bubble */
        for(int i = 2; i < cnt; i++) {
            Move k = mv[i]; int j = i - 1;
            while(j >= 1 && mv[j].score < k.score) { mv[j+1] = mv[j]; j--; }
            mv[j+1] = k;
        }
    }
}

/* ===================== TIME CHECK (SDL) ===================== */
static void check_time(void) {
    if((nodes_count & 1023) == 0) {
        Uint32 elapsed = SDL_GetTicks() - start_time;
        /* v12.9 FIX: the search used to stop only on hard_limit
           (= my_time/5 = up to 20% of the WHOLE game). The target_time
           check ran only BETWEEN depths, so a single long depth iteration
           (depth 14-15 can take 30-120s) ate the whole hard_limit on the
           first move: 5 min game started at ~4:00, 10 min at ~8:00.
           Now the search also aborts mid-iteration once the per-move
           target (my_time/40) is exceeded. Ponder/analysis (target_time=0)
           are unaffected and still use hard_limit only. */
        if(elapsed >= hard_limit) stop_search = 1;
        else if(target_time > 0 && elapsed >= (Uint32)target_time) stop_search = 1;
    }
}

/* ===================== QUIESCENCE (v8.3.1, v12: make/unmake) ===================== */
static int bb_quiescence(BBoard *b,int alpha,int beta,int col_idx,uint64_t hash,int ply,int qsd,const Move *par){
    static const int sgn[2] = {1, -1};
    nodes_count++; if((nodes_count&511)==0)check_time(); if(stop_search)return 0;
    if(ply>=MAX_PLY)return sgn[col_idx]*bb_eval(b,col_idx);
    if(qsd>4) return sgn[col_idx]*bb_eval(b,col_idx);
    line_hashes[ply]=hash;
    for(int i=ply-2;i>=0;i-=2)if(line_hashes[i]==hash)return 0;
    if(is_insufficient_material(b)) return 0;
    int ic=bb_inchk(b,col_idx),sp=0;
    if(!ic){sp=sgn[col_idx]*bb_eval(b,col_idx);
        if(sp+200<alpha) return alpha;
        if(sp>=beta) return beta;
        if(sp>alpha) alpha=sp;
    }
    Move mv[220];int n=bb_gen_moves(b,col_idx,mv);int any=0;Move caps[220];int nc=0;
    { int search_checks = (qsd > 0 && !ic);
    for(int i=0;i<n;i++){UndoInfo _uq; bb_save_undo(b,&mv[i],col_idx,&_uq); bb_do(b,&mv[i],col_idx);
        if(!bb_inchk(b,col_idx)){any=1;
            if(ic||mv[i].cap||mv[i].ep_cap>=0||mv[i].promo) caps[nc++]=mv[i];
            else if(search_checks && bb_inchk(b,col_idx^1)) caps[nc++]=mv[i];}
        bb_undo(b,&mv[i],col_idx,&_uq);}}
    if(!any)return ic?-(MATE-ply):sp;
    for(int i=0;i<nc;i++){int v=caps[i].cap?MAT[caps[i].cap]:(caps[i].promo?MAT[caps[i].promo]:0);int a=abs(bb_piece_at_rc(b,caps[i].fr,caps[i].fc));caps[i].score=mvv_lva(v,a);}
    if(nc > 1) {
        int bi = 0; for(int i=1;i<nc;i++) if(caps[i].score>caps[bi].score) bi=i;
        if(bi){Move t=caps[0];caps[0]=caps[bi];caps[bi]=t;}
        for(int i=2;i<nc;i++){Move k=caps[i];int j=i-1;while(j>=1&&caps[j].score<k.score){caps[j+1]=caps[j];j--;}caps[j+1]=k;}
    }
    for(int i=0;i<nc;i++){
        if(stop_search)return 0;
        if(!ic && caps[i].cap && bb_see(b,&caps[i],col_idx) < 0) continue;
        if(!ic && !caps[i].promo){
            int dv=caps[i].cap?MAT[caps[i].cap]:(caps[i].ep_cap>=0?MAT[PAWN]:0);
            if(sp+dv+200<alpha) continue;
        }
        UndoInfo uq; bb_save_undo(b,&caps[i],col_idx,&uq); bb_do(b,&caps[i],col_idx);
        int gives_chk = (qsd < 4) ? bb_inchk(b, col_idx^1) : 0;
        int nqsd = gives_chk ? qsd + 1 : 0;
        int sc=-bb_quiescence(b,-beta,-alpha,col_idx^1,b->hash,ply+1,nqsd,&caps[i]);
        bb_undo(b,&caps[i],col_idx,&uq);
        if(sc>=beta) return beta; if(sc>alpha) alpha=sc;
    }
    return alpha;
}

/* ===================== NEGAMAX (v8.2, v12: make/unmake) ===================== */
static int bb_negamax(BBoard *b,int depth,int alpha,int beta,int col_idx,uint64_t hash,int ply,const Move *par){
    static const int sgn[2] = {1, -1};
    if(ply>=MAX_PLY)return sgn[col_idx]*bb_eval(b,col_idx);
    if(stop_search)return 0;
    line_hashes[ply]=hash; nodes_count++; check_time();
    int ms=MATE-ply; if(beta>ms)beta=ms; if(alpha<-ms)alpha=-ms; if(alpha>=beta)return alpha;
    if(is_repetition(hash,ply))return 0;
    if(b->fifty>=100)return 0;
    if(is_insufficient_material(b)) return 0;
    int ic=bb_inchk(b,col_idx);
    if(ic) depth=MIN(depth+1,16);
    if(depth<=0) return bb_quiescence(b,alpha,beta,col_idx,hash,ply,0,par);
    int pv=(beta-alpha>1);
    TTEntry *te=&transposition_table[hash&(TT_SIZE-1)];
    int tt_hit=(te->key==hash);
    if(tt_hit&&te->depth>=depth&&!pv&&te->age==tt_age){
        int sc=te->score;if(sc>MATE-200)sc-=ply;if(sc<-MATE+200)sc+=ply;
        if(te->flag==0)return sc; if(te->flag==1&&sc>=beta)return sc; if(te->flag==2&&sc<=alpha)return sc;
    }
    int sp=(tt_hit&&te->flag==0)?te->score:sgn[col_idx]*bb_eval(b,col_idx);
    int hp=(b->pieces[col_idx][BB_N]|b->pieces[col_idx][BB_B]|b->pieces[col_idx][BB_R]|b->pieces[col_idx][BB_Q])!=0;
    if(!ic&&!pv&&depth<=6&&sp-80*depth>=beta&&sp<MATE-200) return sp;
    if(!ic&&!pv&&depth<=2){int mar=(depth==1)?250:450;if(sp+mar<alpha)return bb_quiescence(b,alpha,beta,col_idx,hash,ply,0,par);}
    if(!ic&&!pv&&depth<=3&&sp+120*depth<alpha){
        int r_alpha=alpha-120*depth;
        int v=bb_quiescence(b,r_alpha,r_alpha+1,col_idx,hash,ply,0,par);
        if(v<=r_alpha)return r_alpha;
    }
    /* ProbCut */
    if(!ic&&!pv&&depth>=5&&abs(beta)<MATE-200){
        int rbeta=beta+150;
        Move qmv[220];int qn=bb_gen_moves(b,col_idx,qmv);
        for(int qi=0;qi<qn;qi++){
            if(!qmv[qi].cap&&qmv[qi].ep_cap<0)continue;
            if(qmv[qi].cap&&MAT[qmv[qi].cap]<200&&!qmv[qi].promo)continue;
            if(!ic&&bb_see(b,&qmv[qi],col_idx)<rbeta-sp)continue;
            UndoInfo u_pc; bb_save_undo(b,&qmv[qi],col_idx,&u_pc); bb_do(b,&qmv[qi],col_idx);
            if(bb_inchk(b,col_idx)){bb_undo(b,&qmv[qi],col_idx,&u_pc);continue;}
            int qval=-bb_quiescence(b,-rbeta,-rbeta+1,col_idx^1,b->hash,ply+1,0,&qmv[qi]);
            bb_undo(b,&qmv[qi],col_idx,&u_pc);
            if(qval>=rbeta)return qval;
        }
    }
    /* Null move pruning */
    if(!ic&&!pv&&hp&&depth>=3&&beta<MATE-200&&alpha>-(MATE-200)&&sp>=beta){
        int nm_old_ep=b->ep; uint64_t nm_old_hash=b->hash, nm_old_ph=b->pawn_hash; int nm_old_fifty=b->fifty;
        if(b->ep!=-1){b->hash^=zobrist_ep[b->ep];b->pawn_hash^=zobrist_ep[b->ep];b->ep=-1;}b->fifty=0;xor_side(b);
        int R=3+depth/4+(sp-beta)/200; if(R>depth-1)R=depth-1;
        int ns=-bb_negamax(b,depth-R-1,-beta,-beta+1,col_idx^1,b->hash,ply+1,NULL);
        xor_side(b);b->fifty=nm_old_fifty;b->ep=nm_old_ep;b->hash=nm_old_hash;b->pawn_hash=nm_old_ph;
        if(ns>=beta&&ns<MATE-200){
            if(depth>=6){
                int vs=-bb_negamax(b,depth-4,-beta,-beta+1,col_idx,hash,ply,par);
                if(vs>=beta) return beta;
            } else { return beta; }
        }
    }
    Move mv[220];int n=bb_gen_moves(b,col_idx,mv);
    Move ttm; ttm.fr=-1;
    if(tt_hit&&te->move.fr>=0&&te->move.fr<8) ttm=te->move;
    score_moves(mv,n,col_idx,ply,par,b);
    if(ttm.fr>=0){for(int ii=0;ii<n;ii++)if(mv[ii].fr==ttm.fr&&mv[ii].fc==ttm.fc&&mv[ii].tr==ttm.tr&&mv[ii].tc==ttm.tc){mv[ii].score=1000000;Move t=mv[0];mv[0]=mv[ii];mv[ii]=t;break;}}
    int se_depth=(tt_hit&&ttm.fr>=0&&!ic&&depth>=6&&te->depth>=depth-3&&te->flag==1&&abs(te->score)<MATE-200)?1:0;
    int best=-INF;Move bm;bm.fr=-1;int oa=alpha,leg=0,sr=0;int si2=(col_idx==WC)?0:1;
    static const int FM[5]={0,200,350,500,650};
    /* v12: pre-compute piece types for all moves (needed after make for history) */
    int mv_pt[220]; for(int mi=0;mi<n;mi++) mv_pt[mi]=bb_piece_at(b,col_idx,mv[mi].fr*8+mv[mi].fc);
    /* v12: pre-compute parent target piece for countermove/cmh */
    int par_target_pt = 0;
    if(par&&par->fr>=0) par_target_pt=bb_piece_at_rc(b,par->tr,par->tc);
    for(int ii=0;ii<n;ii++){
        if(stop_search)return 0;
        UndoInfo undo; bb_save_undo(b,&mv[ii],col_idx,&undo); bb_do(b,&mv[ii],col_idx);
        if(bb_inchk(b,col_idx)){bb_undo(b,&mv[ii],col_idx,&undo);continue;}
        leg++;uint64_t nh=b->hash;
        __builtin_prefetch(&transposition_table[nh & (TT_SIZE - 1)]);
        int gc=bb_inchk(b,col_idx^1),ic2=mv[ii].cap||mv[ii].ep_cap>=0,ip=mv[ii].promo;
        int is_quiet=!ic&&!gc&&!ic2&&!ip;
        if(is_quiet&&!pv&&sr>0&&depth<=4&&alpha>-(MATE-200)&&beta<MATE-200)
            if(sp+FM[depth<=4?depth:4]<=alpha){bb_undo(b,&mv[ii],col_idx,&undo);continue;}
        if(is_quiet&&!pv&&depth<=4&&sr>=depth*3+3&&alpha>-(MATE-200)){bb_undo(b,&mv[ii],col_idx,&undo); continue;}
        if(!ic&&ic2&&!ip&&sr>0&&depth<=3&&!pv)
            if(bb_see(b,&mv[ii],col_idx)<-depth*100){bb_undo(b,&mv[ii],col_idx,&undo); continue;}
        if(!ic&&is_quiet&&sr>3&&depth<=3&&!pv)
            if(bb_see(b,&mv[ii],col_idx)<-20*depth){bb_undo(b,&mv[ii],col_idx,&undo); continue;}
        int ext=0;
        if(se_depth&&mv[ii].fr==ttm.fr&&mv[ii].fc==ttm.fc&&mv[ii].tr==ttm.tr&&mv[ii].tc==ttm.tc){
            int sbeta=te->score-depth*2;
            bb_undo(b,&mv[ii],col_idx,&undo);
            int ss2=-bb_negamax(b,depth/2,-sbeta+1,-sbeta,col_idx,hash,ply+1,par);
            bb_save_undo(b,&mv[ii],col_idx,&undo); bb_do(b,&mv[ii],col_idx);
            if(ss2<sbeta) ext=1;
        }
        if(!ext){
            if(mv_pt[ii]==PAWN){
                int f=mv[ii].fc;BB opp_pawns=b->pieces[col_idx^1][BB_P];BB cone=0;
                if(col_idx==WC){for(int nr=mv[ii].tr-1;nr>=0;nr--){BB_SET(cone,nr*8+f);if(f>0)BB_SET(cone,nr*8+f-1);if(f<7)BB_SET(cone,nr*8+f+1);}}
                else{for(int nr=mv[ii].tr+1;nr<8;nr++){BB_SET(cone,nr*8+f);if(f>0)BB_SET(cone,nr*8+f-1);if(f<7)BB_SET(cone,nr*8+f+1);}}
                if(!(opp_pawns&cone))ext=1;
            }
        }
        if(!ext&&mv[ii].cap&&par&&par->cap&&mv[ii].tr*8+mv[ii].tc==par->tr*8+par->tc) ext=1;
        if(!ext){
            if(mv_pt[ii]==PAWN){int rank7=(col_idx==WC)?1:6;if(mv[ii].tr==rank7)ext=1;}
        }
        int red=0;
        if(depth>=3&&sr>=2&&is_quiet){
            red=lmr_table[MIN(depth,63)][MIN(sr,63)];
            int hv=history[si2][mv[ii].fr*8+mv[ii].fc][mv[ii].tr*8+mv[ii].tc];
            if(hv>4000)red=MAX(0,red-2); else if(hv<-2000)red+=1;
            if(mv[ii].score < 0) red++;
            int is_killer=0;
            for(int k=0;k<2&&k<killer_cnt[ply];k++)if(killer[ply][k].fr==mv[ii].fr&&killer[ply][k].fc==mv[ii].fc&&killer[ply][k].tr==mv[ii].tr&&killer[ply][k].tc==mv[ii].tc){is_killer=1;break;}
            if(!is_killer&&!(par&&par->fr>=0&&counter[si2][par->fr*8+par->fc][par->tr*8+par->tc].fr==mv[ii].fr&&counter[si2][par->fr*8+par->fc][par->tr*8+par->tc].fc==mv[ii].fc))red+=1;
            if(par&&par->fr>=0){
                if(par_target_pt!=EMPTY){int opp_abs=abs(par_target_pt);
                    if(opp_abs>=PAWN&&opp_abs<=QUEEN){int opp_pci=(par_target_pt>0)?opp_abs-1:opp_abs+5;
                        int cmh_val=cmh[opp_pci][par->tr*8+par->tc];
                        if(cmh_val>2000)red=MAX(0,red-1);else if(cmh_val<-2000)red+=1;}}}
            if(pv)red=MAX(0,red-1);
            if(red<0)red=0;if(red>=depth-1)red=depth-2;
        }
        int sc;
        if(sr==0&&!tt_hit&&depth>=4&&!ic&&!gc&&!ic2&&!ip){
            sc=-bb_negamax(b,depth-2+ext,-alpha-1,-alpha,col_idx^1,nh,ply+1,&mv[ii]);
            if(sc>alpha)sc=-bb_negamax(b,depth-1+ext,-beta,-alpha,col_idx^1,nh,ply+1,&mv[ii]);
        } else if(sr==0){
            sc=-bb_negamax(b,depth-1+ext,-beta,-alpha,col_idx^1,nh,ply+1,&mv[ii]);
        } else {
            sc=-bb_negamax(b,depth-1-red+ext,-alpha-1,-alpha,col_idx^1,nh,ply+1,&mv[ii]);
            if(sc>alpha&&(red>0||pv))sc=-bb_negamax(b,depth-1+ext,-beta,-alpha,col_idx^1,nh,ply+1,&mv[ii]);
        }
        sr++;
        if(sc>best){best=sc;bm=mv[ii];}
        if(sc>alpha)alpha=sc;
        if(alpha>=beta){
            if(is_quiet){
                int idx=mv[ii].fr*8+mv[ii].fc;int old=history[si2][idx][mv[ii].tr*8+mv[ii].tc];
                history[si2][idx][mv[ii].tr*8+mv[ii].tc]+=depth*depth-old*depth/512;
                if(history[si2][idx][mv[ii].tr*8+mv[ii].tc]>16000)history[si2][idx][mv[ii].tr*8+mv[ii].tc]=16000;
                if(history[si2][idx][mv[ii].tr*8+mv[ii].tc]<-16000)history[si2][idx][mv[ii].tr*8+mv[ii].tc]=-16000;
                for(int j=0;j<ii;j++){if(mv[j].cap||mv[j].ep_cap>=0||mv[j].promo)continue;
                    int jidx=mv[j].fr*8+mv[j].fc;history[si2][jidx][mv[j].tr*8+mv[j].tc]-=depth*depth;
                    if(history[si2][jidx][mv[j].tr*8+mv[j].tc]<-16000)history[si2][jidx][mv[j].tr*8+mv[j].tc]=-16000;}
                if(par&&par->fr>=0){
                    if(par_target_pt!=EMPTY){int opp_abs=abs(par_target_pt);
                        if(opp_abs>=PAWN&&opp_abs<=QUEEN){int opp_pci=(par_target_pt>0)?opp_abs-1:opp_abs+5;
                            cmh[opp_pci][par->tr*8+par->tc]-=depth;if(cmh[opp_pci][par->tr*8+par->tc]<-8192)cmh[opp_pci][par->tr*8+par->tc]=-8192;}}}
                int cur_pt=mv_pt[ii];int cur_pci=(col_idx==WC)?cur_pt-1:cur_pt+5;int prev_pci=0;
                if(par&&par->fr>=0){int prev_pt=par_target_pt;
                    if(prev_pt>=PAWN&&prev_pt<=QUEEN)prev_pci=((col_idx^1)==WC)?prev_pt-1:prev_pt+5;}
                cont_hist[prev_pci][cur_pci][mv[ii].tr*8+mv[ii].tc]+=depth*depth;
                if(cont_hist[prev_pci][cur_pci][mv[ii].tr*8+mv[ii].tc]>16384)cont_hist[prev_pci][cur_pci][mv[ii].tr*8+mv[ii].tc]=16384;
                for(int j=0;j<ii;j++){if(mv[j].cap||mv[j].ep_cap>=0||mv[j].promo)continue;
                    int jp=mv_pt[j];int jpci=(col_idx==WC)?jp-1:jp+5;int jprev_pci=0;
                    if(par&&par->fr>=0){int jprev_pt=par_target_pt;
                        if(jprev_pt>=PAWN&&jprev_pt<=QUEEN)jprev_pci=((col_idx^1)==WC)?jprev_pt-1:jprev_pt+5;}
                    cont_hist[jprev_pci][jpci][mv[j].tr*8+mv[j].tc]-=depth*depth;
                    if(cont_hist[jprev_pci][jpci][mv[j].tr*8+mv[j].tc]<-16384)cont_hist[jprev_pci][jpci][mv[j].tr*8+mv[j].tc]=-16384;}
                if(killer_cnt[ply]<2)killer[ply][killer_cnt[ply]++]=mv[ii];
                else{killer[ply][0]=killer[ply][1];killer[ply][1]=mv[ii];}
                if(par&&par->fr>=0)counter[si2][par->fr*8+par->fc][par->tr*8+par->tc]=mv[ii];
                if(par&&par->fr>=0){
                    if(par_target_pt!=EMPTY){int opp_abs=abs(par_target_pt);
                        if(opp_abs>=PAWN&&opp_abs<=QUEEN){int opp_pci=(par_target_pt>0)?opp_abs-1:opp_abs+5;
                            cmh[opp_pci][par->tr*8+par->tc]+=depth*depth;if(cmh[opp_pci][par->tr*8+par->tc]>8192)cmh[opp_pci][par->tr*8+par->tc]=8192;}}}
            }
            if(!is_quiet&&(mv[ii].cap||mv[ii].ep_cap>=0)){
                int cap_piece=mv_pt[ii];
                int cap_pci=(col_idx==WC)?cap_piece-1:cap_piece+5;
                int old_ch=cap_history[cap_pci][mv[ii].tr*8+mv[ii].tc];
                cap_history[cap_pci][mv[ii].tr*8+mv[ii].tc]+=depth*depth-old_ch*depth/512;
                if(cap_history[cap_pci][mv[ii].tr*8+mv[ii].tc]>16000)cap_history[cap_pci][mv[ii].tr*8+mv[ii].tc]=16000;
            }
            bb_undo(b,&mv[ii],col_idx,&undo);
            break;
        }
        bb_undo(b,&mv[ii],col_idx,&undo);
    }
    if(!leg)return ic?-(MATE-ply):0;
    int ss=best;if(ss>MATE-200)ss+=ply;if(ss<-MATE+200)ss-=ply;
    if(bm.fr>=0){te->key=hash;te->depth=depth;te->score=ss;te->move=bm;te->flag=(best<=oa)?2:(best>=beta)?1:0;te->age=tt_age;}
    return best;
}


/* ===================== BOOK ===================== */
#ifdef USE_BOOK
static uint64_t lcg_st=42;
static uint64_t BR[781];
static void init_br(void){
    for(int i=0;i<781;i++){lcg_st=lcg_st*6364136223846793005ULL+1442695040888963407ULL;BR[i]=lcg_st;}
}
static uint64_t bk_hash_bb(BBoard *b, int side){
    uint64_t h=0;
    for(int col=0;col<2;col++) for(int t=0;t<6;t++){
        BB bb=b->pieces[col][t];
        while(bb){int sq=BB_LSB(bb);BB_POP(bb);
            int tp=t+1; int cl=(col==WC)?1:0;
            int pidx=(tp-1)*2+cl; h^=BR[pidx*64+sq];}
    }
    if(b->cr&CR_WK)h^=BR[768]; if(b->cr&CR_WQ)h^=BR[769];
    if(b->cr&CR_BK)h^=BR[770]; if(b->cr&CR_BQ)h^=BR[771];
    if(b->ep>=0) h^=BR[772+(b->ep)]; /* ep is file 0-7 */
    if(side==WHITE) h^=BR[780];
    return h;
}
static Move book_move(void){
    Move m; m.fr=-1;
    uint64_t key=bk_hash_bb(&B,turn);
    int lo=0,hi=BOOK_SIZE-1,first=-1;
    while(lo<=hi){int mid=(lo+hi)/2;
        if(BOOK_DATA[mid].key==key){first=mid;while(first>0&&BOOK_DATA[first-1].key==key)first--;break;}
        else if(BOOK_DATA[mid].key<key)lo=mid+1;else hi=mid-1;}
    if(first==-1) return m;
    Move book_moves[256]; int book_count=0;
    for(int i=first;i<BOOK_SIZE&&BOOK_DATA[i].key==key;i++){
        const BK*e=&BOOK_DATA[i];
        Move tmp[256]; int n=bb_gen_moves(&B, turn==WHITE?WC:BC, tmp);
        for(int j=0;j<n;j++){
            if(tmp[j].fr==e->fr&&tmp[j].fc==e->fc&&tmp[j].tr==e->tr&&tmp[j].tc==e->tc){
                BBoard bc; memcpy(&bc,&B,sizeof bc);
                bb_do(&bc,&tmp[j],turn==WHITE?WC:BC);
                if(!bb_inchk(&bc,turn==WHITE?WC:BC)){book_moves[book_count++]=tmp[j];}
                break;
            }
        }
    }
    if(book_count>0){int idx=rand()%book_count; return book_moves[idx];}
    return m;
}
#endif

/* ===================== INIT BOARD (v8.2 pawn_hash) ===================== */
static void init_pawn_hash(BBoard *b) {
    b->pawn_hash = 0;
    for(int col=0; col<2; col++) {
        BB pp = b->pieces[col][BB_P];
        while(pp) { int sq = BB_LSB(pp); BB_POP(pp);
            b->pawn_hash ^= zobrist_pieces[BB_P*2+col][sq]; }
    }
    if(b->ep >= 0) b->pawn_hash ^= zobrist_ep[b->ep];
}

static void uci_dbg_open(void);
static void uci_dbg_log(const char *dir, int ei, const char *line);
static void init_board(void){
    memset(&B,0,sizeof B);
    for(int c=0;c<8;c++){
        int piece=(c==0||c==7)?ROOK:(c==1||c==6)?KNIGHT:(c==2||c==5)?BISHOP:(c==3)?QUEEN:KING;
        BB_SET(B.pieces[BC][piece-1],c); BB_SET(B.pieces[BC][BB_P],8+c);
    }
    for(int c=0;c<8;c++){
        int piece=(c==0||c==7)?ROOK:(c==1||c==6)?KNIGHT:(c==2||c==5)?BISHOP:(c==3)?QUEEN:KING;
        BB_SET(B.pieces[WC][piece-1],56+c); BB_SET(B.pieces[WC][BB_P],48+c);
    }
    bb_sync_occ(&B);
    B.cr=CR_WK|CR_WQ|CR_BK|CR_BQ; B.ep=-1; B.fifty=0; B.hash=0;
    for(int col=0;col<2;col++) for(int t=0;t<6;t++){
        BB bb=B.pieces[col][t];
        while(bb){int sq=BB_LSB(bb);BB_POP(bb);B.hash^=zobrist_pieces[t*2+col][sq];}
    }
    xor_cr(&B,B.cr);
    init_pawn_hash(&B);
    xor_side(&B); /* white to move */
    clk_w = clk_b = base_time;
    last_ms=SDL_GetTicks();
    clock_started=0; /* v12.9: freeze clocks until the first move */
    { char _dbg[128]; snprintf(_dbg,sizeof _dbg,
        "init_board: base_time=%u ms (%u:%02u) -> clk_w=clk_b=%u",
        base_time, base_time/60000, (base_time/1000)%60, clk_w);
      uci_dbg_log("CLOCK", -1, _dbg); }
    lm_fr=lm_fc=lm_tr=lm_tc=-1;
    promo_pending=0;
    memset(cap_w,0,sizeof cap_w); memset(cap_b,0,sizeof cap_b);
    hist_n=0; draw_offered=0; game_over=0; sel_r=sel_c=-1; anim_active=0; drag_active=0;
    /* sync game_hist_hashes */
    game_hist_n=0;
    if(game_hist_n<MAX_GAME_HIST) game_hist_hashes[game_hist_n++]=B.hash;
}

/* ===================== FEN PARSER ===================== */
static int parse_fen(const char *fen){
    /* Returns 1 on success, 0 on error */
    char buf[512]; strncpy(buf,fen,511); buf[511]=0;
    memset(&B,0,sizeof B);
    /* 1) Piece placement */
    char *tok=strtok(buf," "); if(!tok) return 0;
    int rank=0, file=0;
    for(int i=0;tok[i];i++){
        char c=tok[i];
        if(c=='/'){rank++;file=0;if(rank>7)return 0;continue;}
        if(c>='1'&&c<='8'){file+=c-'0';if(file>8)return 0;continue;}
        int col=-1,tp=-1;
        switch(c){
            case 'P':col=WC;tp=BB_P;break; case 'N':col=WC;tp=BB_N;break;
            case 'B':col=WC;tp=BB_B;break; case 'R':col=WC;tp=BB_R;break;
            case 'Q':col=WC;tp=BB_Q;break; case 'K':col=WC;tp=BB_K;break;
            case 'p':col=BC;tp=BB_P;break; case 'n':col=BC;tp=BB_N;break;
            case 'b':col=BC;tp=BB_B;break; case 'r':col=BC;tp=BB_R;break;
            case 'q':col=BC;tp=BB_Q;break; case 'k':col=BC;tp=BB_K;break;
            default:return 0;
        }
        if(col<0||tp<0||file>7)return 0;
        BB_SET(B.pieces[col][tp],rank*8+file); file++;
    }
    if(file!=8||rank!=7)return 0;
    bb_sync_occ(&B);
    /* 2) Side to move */
    tok=strtok(NULL," "); if(!tok) return 0;
    turn=(tok[0]=='b')?BLACK:WHITE;
    /* 3) Castling */
    tok=strtok(NULL," "); if(!tok) return 0;
    B.cr=0;
    if(strcmp(tok,"-")!=0){
        for(int i=0;tok[i];i++){
            if(tok[i]=='K')B.cr|=CR_WK; else if(tok[i]=='Q')B.cr|=CR_WQ;
            else if(tok[i]=='k')B.cr|=CR_BK; else if(tok[i]=='q')B.cr|=CR_BQ;
        }
    }
    /* 4) En passant — validate that the ep square is actually possible */
    tok=strtok(NULL," "); if(!tok) return 0;
    B.ep=-1;
    if(strcmp(tok,"-")!=0&&strlen(tok)==2){
        int fc2=tok[0]-'a', fr2=tok[1]-'0'-1;
        if(fc2>=0&&fc2<8&&fr2>=0&&fr2<8){
            // Accept en passant square as given in FEN (more lenient)
            B.ep = fc2;
        }
    }
    /* 5) Halfmove clock (optional) */
    tok=strtok(NULL," ");
    B.fifty=0;
    if(tok) B.fifty=atoi(tok);
    /* Build hash */
    B.hash=0;
    for(int col=0;col<2;col++) for(int t=0;t<6;t++){
        BB bb=B.pieces[col][t];
        while(bb){int sq=BB_LSB(bb);BB_POP(bb);B.hash^=zobrist_pieces[t*2+col][sq];}
    }
    xor_cr(&B,B.cr);
    if(B.ep>=0) B.hash^=zobrist_ep[B.ep];
    if(turn==BLACK) xor_side(&B);
    init_pawn_hash(&B);
    return 1;
}

/* ===================== FEN GENERATOR (v14) ===================== */
static void board_to_fen(char *out, size_t maxlen){
    int pos=0;
    for(int r=0;r<8;r++){
        int empty=0;
        for(int c=0;c<8;c++){
            int sq=r*8+c;
            int piece=0;
            for(int col=0;col<2;col++) for(int t=0;t<6;t++) if(BB_GET(B.pieces[col][t],sq)) piece=(col==WC?1:-1)*(t+1);
            if(piece==0) empty++;
            else{
                if(empty){ if(pos<(int)maxlen-2) out[pos++]='0'+empty; empty=0; }
                char ch='?';
                switch(abs(piece)){case PAWN:ch='p';break;case KNIGHT:ch='n';break;case BISHOP:ch='b';break;case ROOK:ch='r';break;case QUEEN:ch='q';break;case KING:ch='k';break;}
                if(piece>0) ch=toupper((unsigned char)ch);
                if(pos<(int)maxlen-1) out[pos++]=ch;
            }
        }
        if(empty){ if(pos<(int)maxlen-1) out[pos++]='0'+empty; }
        if(r!=7 && pos<(int)maxlen-1) out[pos++]='/';
    }
    if(pos<(int)maxlen-1) out[pos++]=' ';
    if(pos<(int)maxlen-1) out[pos++]=(turn==WHITE?'w':'b');
    if(pos<(int)maxlen-1) out[pos++]=' ';
    if(B.cr==0){ if(pos<(int)maxlen-1) out[pos++]='-'; }
    else{
        if(B.cr&CR_WK && pos<(int)maxlen-1) out[pos++]='K';
        if(B.cr&CR_WQ && pos<(int)maxlen-1) out[pos++]='Q';
        if(B.cr&CR_BK && pos<(int)maxlen-1) out[pos++]='k';
        if(B.cr&CR_BQ && pos<(int)maxlen-1) out[pos++]='q';
    }
    if(pos<(int)maxlen-1) out[pos++]=' ';
    if(B.ep>=0){
        char file='a'+B.ep;
        char rank=(turn==WHITE?'6':'3');
        if(pos<(int)maxlen-1) out[pos++]=file;
        if(pos<(int)maxlen-1) out[pos++]=rank;
    } else {
        if(pos<(int)maxlen-1) out[pos++]='-';
    }
    {
        char tmp[16]; int n=snprintf(tmp,sizeof tmp," %d",B.fifty);
        for(int i=0;i<n && pos<(int)maxlen-1;i++) out[pos++]=tmp[i];
    }
    {
        int fm = hist_n/2 + 1;
        char tmp[16]; int n=snprintf(tmp,sizeof tmp," %d",fm);
        for(int i=0;i<n && pos<(int)maxlen-1;i++) out[pos++]=tmp[i];
    }
    out[pos]=0;
}
static char cached_fen[256]="";
static void update_cached_fen(void){ board_to_fen(cached_fen,sizeof cached_fen); }
static void copy_fen_to_clipboard(void){
    update_cached_fen();
    if(SDL_SetClipboardText(cached_fen)==0) bottom_log_push("FEN copied to clipboard");
    else bottom_log_push("FEN copy failed");
    uci_dbg_log("FEN", -1, cached_fen);
}
static int fen_panel_x=0, fen_panel_y=0, fen_panel_w=0, fen_panel_h=0;

/* ===================== DRAW HELPERS ===================== */
static void frect(int x,int y,int w,int h,int R,int Gv,int B){
    SDL_SetRenderDrawColor(ren,R,Gv,B,255);
    SDL_Rect rc={x,y,w,h};
    SDL_RenderFillRect(ren,&rc);
}
static void orect(int x,int y,int w,int h,int R,int Gv,int B){
    SDL_SetRenderDrawColor(ren,R,Gv,B,255);
    SDL_Rect rc={x,y,w,h};
    SDL_RenderDrawRect(ren,&rc);
}
static void fcircle(int cx,int cy,int r,int R,int Gv,int B){
    SDL_SetRenderDrawColor(ren,R,Gv,B,255);
    for(int dy=-r;dy<=r;dy++){
        int dx=(int)sqrt((double)(r*r-dy*dy));
        SDL_RenderDrawLine(ren,cx-dx,cy+dy,cx+dx,cy+dy);
    }
}
static void draw_arrow(int x1, int y1, int x2, int y2, int cr, int cg, int cb, int alpha) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, cr, cg, cb, alpha);
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrt(dx*dx + dy*dy);
    if (len < 1.0f) { SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE); return; }
    float ux = dx/len, uy = dy/len;
    float nx = -uy, ny = ux;
    int thickness = MAX(3, SQ_SIZE/10);
    /* Shorten start slightly */
    float start_off = SQ_SIZE * 0.15f;
    float sx = x1 + ux * start_off, sy = y1 + uy * start_off;
    /* Shaft ends before arrowhead */
    float head_len = SQ_SIZE * 0.35f;
    float ex = x2 - ux * head_len, ey = y2 - uy * head_len;
    for (int i = -thickness/2; i <= thickness/2; i++) {
        int ox = (int)(nx * i), oy = (int)(ny * i);
        SDL_RenderDrawLine(ren, (int)sx+ox, (int)sy+oy, (int)ex+ox, (int)ey+oy);
    }
    /* Arrowhead triangle */
    float hw = SQ_SIZE * 0.28f;
    int h1x = (int)(x2 - ux*head_len + nx*hw);
    int h1y = (int)(y2 - uy*head_len + ny*hw);
    int h2x = (int)(x2 - ux*head_len - nx*hw);
    int h2y = (int)(y2 - uy*head_len - ny*hw);
    /* Fill arrowhead with scanlines */
    int mny = MIN(y2, MIN(h1y, h2y));
    int mxy = MAX(y2, MAX(h1y, h2y));
    for (int yy = mny; yy <= mxy; yy++) {
        float left = (float)x2, right = (float)x2;
        if (yy >= MIN(y2,h1y) && yy <= MAX(y2,h1y) && h1y != y2) {
            float t = (float)(yy - y2) / (float)(h1y - y2);
            float xi = x2 + t * (h1x - x2);
            if (xi < left) left = xi; if (xi > right) right = xi;
        }
        if (yy >= MIN(y2,h2y) && yy <= MAX(y2,h2y) && h2y != y2) {
            float t = (float)(yy - y2) / (float)(h2y - y2);
            float xi = x2 + t * (h2x - x2);
            if (xi < left) left = xi; if (xi > right) right = xi;
        }
        if (yy >= MIN(h1y,h2y) && yy <= MAX(h1y,h2y) && h2y != h1y) {
            float t = (float)(yy - h1y) / (float)(h2y - h1y);
            float xi = h1x + t * (h2x - h1x);
            if (xi < left) left = xi; if (xi > right) right = xi;
        }
        SDL_RenderDrawLine(ren, (int)left, yy, (int)right, yy);
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

static const unsigned char PF[128][9]={
    [' ']={0},
    ['0']={0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C},['1']={0x18,0x38,0x18,0x18,0x18,0x18,0x7E},
    ['2']={0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E},['3']={0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C},
    ['4']={0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C},['5']={0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C},
    ['6']={0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C},['7']={0x7E,0x06,0x0C,0x18,0x30,0x30,0x30},
    ['8']={0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C},['9']={0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38},
    ['a']={0,0,0x3C,0x06,0x3E,0x66,0x3E},['b']={0x60,0x60,0x7C,0x66,0x66,0x66,0x7C},
    ['c']={0,0,0x3C,0x60,0x60,0x60,0x3C},['d']={0x06,0x06,0x3E,0x66,0x66,0x66,0x3E},
    ['e']={0,0,0x3C,0x66,0x7E,0x60,0x3C},['f']={0x1C,0x30,0x7C,0x30,0x30,0x30,0x30},
    ['g']={0,0,0x3E,0x66,0x66,0x3E,0x06,0x7C},['h']={0x60,0x60,0x7C,0x66,0x66,0x66,0x66},
    ['i']={0x18,0,0x38,0x18,0x18,0x18,0x3C},['j']={0x06,0,0x06,0x06,0x06,0x66,0x3C},
    ['k']={0x60,0x66,0x6C,0x78,0x6C,0x66,0x66},['l']={0x38,0x18,0x18,0x18,0x18,0x18,0x3C},
    ['m']={0,0,0x66,0x7F,0x6B,0x63,0x63},['n']={0,0,0x7C,0x66,0x66,0x66,0x66},
    ['o']={0,0,0x3C,0x66,0x66,0x66,0x3C},['p']={0,0,0x7C,0x66,0x66,0x7C,0x60,0x60},
    ['q']={0,0,0x3E,0x66,0x66,0x3E,0x06,0x06},['r']={0,0,0x7C,0x66,0x60,0x60,0x60},
    ['s']={0,0,0x3E,0x60,0x3C,0x06,0x7C},['t']={0x30,0x30,0x7C,0x30,0x30,0x30,0x1C},
    ['u']={0,0,0x66,0x66,0x66,0x66,0x3E},['v']={0,0,0x66,0x66,0x66,0x3C,0x18},
    ['w']={0,0,0x63,0x63,0x6B,0x7F,0x36},['x']={0,0,0x66,0x3C,0x18,0x3C,0x66},
    ['y']={0,0,0x66,0x66,0x3E,0x06,0x3C},['z']={0,0,0x7E,0x0C,0x18,0x30,0x7E},
    ['A']={0x18,0x3C,0x66,0x7E,0x66,0x66,0x66},['B']={0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C},
    ['C']={0x3C,0x66,0x60,0x60,0x60,0x66,0x3C},['D']={0x78,0x6C,0x66,0x66,0x66,0x6C,0x78},
    ['E']={0x7E,0x60,0x60,0x78,0x60,0x60,0x7E},['F']={0x7E,0x60,0x60,0x78,0x60,0x60,0x60},
    ['G']={0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C},['H']={0x66,0x66,0x66,0x7E,0x66,0x66,0x66},
    ['I']={0x7E,0x18,0x18,0x18,0x18,0x18,0x7E},['J']={0x06,0x06,0x06,0x06,0x66,0x66,0x3C},
    ['K']={0x66,0x6C,0x78,0x70,0x78,0x6C,0x66},['L']={0x60,0x60,0x60,0x60,0x60,0x60,0x7E},
    ['M']={0x63,0x77,0x7F,0x6B,0x63,0x63,0x63},['N']={0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66},
    ['O']={0x3C,0x66,0x66,0x66,0x66,0x66,0x3C},['P']={0x7C,0x66,0x66,0x7C,0x60,0x60,0x60},
    ['Q']={0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06},['R']={0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66},
    ['S']={0x3E,0x60,0x60,0x3C,0x06,0x06,0x7C},['T']={0x7E,0x18,0x18,0x18,0x18,0x18,0x18},
    ['U']={0x66,0x66,0x66,0x66,0x66,0x66,0x3C},['V']={0x66,0x66,0x66,0x66,0x3C,0x3C,0x18},
    ['W']={0x63,0x63,0x63,0x6B,0x7F,0x77,0x63},['X']={0x66,0x66,0x3C,0x18,0x3C,0x66,0x66},
    ['Y']={0x66,0x66,0x3C,0x18,0x18,0x18,0x18},['Z']={0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E},
    [':']={0,0x18,0x18,0,0x18,0x18},['+']={0,0x18,0x18,0x7E,0x18,0x18},
    ['-']={0,0,0,0,0,0x7E},['.']=      {0,0,0,0,0,0,0x18,0x18},
    ['(']={0x0C,0x18,0x30,0x30,0x30,0x18,0x0C},[')']=  {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30},
    ['=']={0,0,0x7E,0,0,0x7E},['/']=  {0x02,0x06,0x0C,0x18,0x30,0x60,0x40},
    ['!']={0x18,0x18,0x18,0x18,0,0,0,0x18},['?']={0x3C,0x66,0x06,0x0C,0x18,0,0,0x18},
    ['_']={0,0,0,0,0,0,0x7E},['*']= {0,0,0x66,0x3C,0xFF,0x3C,0x66,0},
};
static void dtxt(int x,int y,const char*t,int sc,int R,int Gv,int B){
    (void)sc;
    ttf_draw(font_ui,1,x,y,t,R,Gv,B);
}
static void dtxt_raw(int x,int y,const char*t,int sc,int R,int Gv,int B){
    (void)sc;
    ttf_draw(font_raw,0,x,y,t,R,Gv,B);
}
static void sq2px(int r,int c,int*px,int*py){
    int fl=flip_board^(player_color==BLACK?1:0);
    *px=BOARD_OX+(fl?7-c:c)*SQ_SIZE;
    *py=BOARD_OY+(fl?7-r:r)*SQ_SIZE;
}
static void px2sq(int mx,int my,int*r,int*c){
    int fl=flip_board^(player_color==BLACK?1:0);
    int vc=(mx-BOARD_OX)/SQ_SIZE, vr=(my-BOARD_OY)/SQ_SIZE;
    if(vc<0||vc>7||vr<0||vr>7){*r=*c=-1;return;}
    *c=fl?7-vc:vc; *r=fl?7-vr:vr;
}
static void draw_piece_at(int piece,int row,int col){
    int wh=(piece>0),tp=abs(piece);
    int px,py; sq2px(row,col,&px,&py);
    if(0){
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren,0,0,0,45);
        int cx=px+SQ_SIZE/2, cy=py+SQ_SIZE - 8;
        int rx=SQ_SIZE/3, ry=SQ_SIZE/8;
        for(int dy=-ry; dy<=ry; dy++){
            int dx=(int)(rx * sqrt(1.0 - (double)dy*dy/(ry*ry+1)));
            SDL_RenderDrawLine(ren,cx-dx,cy+dy,cx+dx,cy+dy);
        }
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    }
    if(retro_piece_style){
        /* high-contrast retro: thicker outline */
        SDL_SetRenderDrawColor(ren,0,0,0,90);
        SDL_Rect sh={px+7,py+7,SQ_SIZE-10,SQ_SIZE-10};
        SDL_RenderFillRect(ren,&sh);
    }
    if(tex[wh?1:0][tp]){
        SDL_Rect d={px+4,py+4,SQ_SIZE-8,SQ_SIZE-8};
        SDL_RenderCopy(ren,tex[wh?1:0][tp],NULL,&d);
    } else {
        fcircle(px+SQ_SIZE/2,py+SQ_SIZE/2,SQ_SIZE/2-8, wh?80:10,wh?60:10,wh?30:10);
        fcircle(px+SQ_SIZE/2,py+SQ_SIZE/2,SQ_SIZE/2-10,wh?245:35,wh?242:32,wh?220:28);
    }
}
/* v12: same as draw_piece_at but at explicit pixel coords (top-left of square) —
   used for the slide animation and for the piece following the cursor while dragging */
static void draw_piece_px(int piece,int px,int py){
    if(piece==0) return;
    int wh=(piece>0),tp=abs(piece);
    if(0){
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren,0,0,0,45);
        int cx=px+SQ_SIZE/2, cy=py+SQ_SIZE - 8;
        int rx=SQ_SIZE/3, ry=SQ_SIZE/8;
        for(int dy=-ry; dy<=ry; dy++){
            int dx=(int)(rx * sqrt(1.0 - (double)dy*dy/(ry*ry+1)));
            SDL_RenderDrawLine(ren,cx-dx,cy+dy,cx+dx,cy+dy);
        }
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    }
    if(tex[wh?1:0][tp]){
        SDL_Rect d={px+4,py+4,SQ_SIZE-8,SQ_SIZE-8};
        SDL_RenderCopy(ren,tex[wh?1:0][tp],NULL,&d);
    } else {
        fcircle(px+SQ_SIZE/2,py+SQ_SIZE/2,SQ_SIZE/2-8, wh?80:10,wh?60:10,wh?30:10);
        fcircle(px+SQ_SIZE/2,py+SQ_SIZE/2,SQ_SIZE/2-10,wh?245:35,wh?242:32,wh?220:28);
    }
}
static void seg7(int x,int y,int d,int R,int Gv,int B){
    /* FIX: table[4] was a duplicate of table[3] ("3" pattern), pushing the
       real patterns for 4-9 one slot too far right. Every digit >=4 was
       rendered as the PREVIOUS digit (5 looked like 4, 6 like 5, ...) --
       this is what made a "5 min" clock read "4:00". Corrected to the
       standard 0-9 seven-segment patterns (segment order: top, upper-left,
       upper-right, middle, lower-left, lower-right, bottom). */
    int s[10][7]={{1,1,1,0,1,1,1},{0,0,1,0,0,1,0},{1,0,1,1,1,0,1},{1,0,1,1,0,1,1},{0,1,1,1,0,1,0},{1,1,0,1,0,1,1},{1,1,0,1,1,1,1},{1,0,1,0,0,1,0},{1,1,1,1,1,1,1},{1,1,1,1,0,1,1}};
    if(d<0)d=0; if(d>9)d=9; /* guard: m/10 can exceed 9 for 100+ minute clocks */
    int sz=5,w=sz*3,h=sz*5;SDL_SetRenderDrawColor(ren,R,Gv,B,255);
    SDL_Rect sg[7]={{x,y,w,sz},{x,y,sz,h/2},{x+w-sz,y,sz,h/2},{x,y+h/2,w,sz},{x,y+h/2,sz,h/2},{x+w-sz,y+h/2,sz,h/2},{x,y+h-sz,w,sz}};
    for(int i=0;i<7;i++)if(s[d][i])SDL_RenderFillRect(ren,&sg[i]);
}
static void draw_clock(int x,int y,Uint32 ms,int active){
    int R=active?220:90,Gv=active?220:90,B=active?220:90;
    if(ms<30000){R=220;Gv=active?120:60;B=active?20:10;}
    int t=(int)(ms/1000),m=t/60,s=t%60;
    frect(x-6,y-4,22*4+26,30,active?30:15,active?30:15,active?30:15);
    if(active)orect(x-6,y-4,22*4+26,30,R,Gv,B);
    seg7(x,y,m/10,R,Gv,B);seg7(x+22,y,m%10,R,Gv,B);
    SDL_SetRenderDrawColor(ren,R,Gv,B,255);SDL_Rect d1={x+22*2+3,y+5,3,5},d2={x+22*2+3,y+15,3,5};SDL_RenderFillRect(ren,&d1);SDL_RenderFillRect(ren,&d2);
    seg7(x+22*2+9,y,s/10,R,Gv,B);seg7(x+22*3+9,y,s%10,R,Gv,B);
}


/* ===================== PROMO POPUP ===================== */
static void render_promo(void){
    int pieces[4]={QUEEN,ROOK,BISHOP,KNIGHT},pw=SQ_SIZE,gap=8,total=4*(pw+gap)-gap;
    int px=(WIN_W-total)/2,py=WIN_H/2-pw/2;
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,0,0,0,160);SDL_Rect ov={0,0,WIN_W,WIN_H};SDL_RenderFillRect(ren,&ov);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    frect(px-10,py-26,total+20,pw+36,35,35,42);orect(px-10,py-26,total+20,pw+36,120,120,160);
    dtxt(px+8,py-20,"Promote to:",1,200,200,220);
    for(int i=0;i<4;i++){int bx=px+i*(pw+gap);
        frect(bx,py,pw,pw,50,50,62);orect(bx,py,pw,pw,130,130,170);
        int p=(player_color==WHITE)?pieces[i]:-pieces[i];
        if(tex[(p>0)?1:0][abs(p)]){SDL_Rect d={bx+4,py+4,pw-8,pw-8};SDL_RenderCopy(ren,tex[(p>0)?1:0][abs(p)],NULL,&d);}
    }
    SDL_RenderPresent(ren);
}
static int promo_click(int mx,int my){
    int pieces[4]={QUEEN,ROOK,BISHOP,KNIGHT},pw=SQ_SIZE,gap=8,total=4*(pw+gap)-gap;
    int px=(WIN_W-total)/2,py=WIN_H/2-pw/2;
    for(int i=0;i<4;i++){int bx=px+i*(pw+gap);if(mx>=bx&&mx<=bx+pw&&my>=py&&my<=py+pw)return pieces[i];}
    return 0;
}

/* ===================== MENUS ===================== */
/* ===================== v9: MENUS ===================== */
#define N_MENUS 6
static const char*MNAME[N_MENUS]={"Game","Settings","Engine","Tournament","Options","Help"};
static const char*GITEMS[]={
    "New game (White)","New game (Black)","AI vs AI",
    "Load FEN  (L)","Load game","Paste FEN (Ctrl+V)","Undo move (U)",
    "Draw offer","Resign","Save game",
    "Replay: Prev (Left)","Replay: Next (Right)","Replay: First (Home)","Replay: Last (End)",
    "Replay All (G)"
};
#define N_GAME 15
static const char*SITEMS[]={
    "Sound on/off  (M)",
    "Flip board  (F)",
    "Opening book on/off",
    "--- Themes (CMD) ---",
    "CMD Gray","CMD High Contrast","CMD Amber",
    "CMD Red Tint","CMD Blue","Classic Green","Classic Brown","Classic Gray",
    "--- Time ---",
    "1 min","3 min","5 min","10 min","30 min",
    "60 min","120 min",
    "--- Increment ---",
    "5+3 (5 min + 3 sec)","4+2 (4 min + 2 sec)",
    "--- Ponder ---",
    "Ponder on/off"
};
#define N_SET (sizeof(SITEMS)/sizeof(SITEMS[0]))
static const char*EITEMS[]={
    "--- Engine Select ---",            /* 0  sep */
    "Built-in StrongEngine",           /* 1       */
    "--- UCI Engine 1 ---",            /* 2  sep */
    "Browse Engine 1...",              /* 3       */
    "--- UCI Engine 2 ---",            /* 4  sep */
    "Browse Engine 2...",              /* 5       */
};
#define N_ENG (sizeof(EITEMS)/sizeof(EITEMS[0]))
static const char*TITEMS[]={
    "--- Games ---",
    "2 games","4 games","10 games","20 games","Custom games...",
    "--- White player ---",
    "W: Built-in","W: UCI Engine 1","W: UCI Engine 2","W: Human",
    "--- Black player ---",
    "B: Built-in","B: UCI Engine 1","B: UCI Engine 2","B: Human",
    "--- Actions ---",
    "Start tournament","Stop tournament",
    "--- Multi-engine ---",
    "Tournament Manager..."
};
#define N_TOUR (sizeof(TITEMS)/sizeof(TITEMS[0]))
static const char*HITEMS[]={
    "--- Keyboard ---",
    "U        Undo last move",
    "F        Flip board view",
    "M        Toggle sound",
    "R        New game (White)",
    "I        Toggle analysis",
    "L        Load position (FEN)",
    "T        Start tournament",
    "G        Replay all (auto-play)",
    "Left/Right  Replay prev/next",
    "Home/End   First/last move",
    "Ctrl+S    Save game",
    "Y/Ctrl+C Copy FEN (FEN panel)",
    "--- Mouse ---",
    "Drag piece   Move a piece",
    "Right-drag   Draw arrow",
    "A          Clear drawn arrows",
    "Click FEN panel to copy FEN",
    "--- Menus ---",
    "Game      New / Load / Replay",
    "Settings  Theme, Time, Ponder",
    "Engine    Add / manage UCI",
    "C         Copy visible tab"
};
#define N_HELP 23
static const char*OITEMS[]={
    "Engine 1 options...",
    "Engine 2 options...",
};
#define N_OPT (sizeof(OITEMS)/sizeof(OITEMS[0]))
static int menu_count(int mi){
    if(mi==4) return (int)N_OPT; /* v12.3: just two launcher items now, see uci_opts_dialog */
    return mi==0?N_GAME:mi==1?N_SET:mi==2?(int)N_ENG:mi==3?(int)N_TOUR:N_HELP;
}
static const char*menu_item(int mi,int ii){
    if(mi==4) return OITEMS[ii];
    if(mi==3&&ii==5){static char mb[24]; snprintf(mb,sizeof mb,"Custom games... (%d)", tourney_total); return mb;}
    return mi==0?GITEMS[ii]:mi==1?SITEMS[ii]:mi==2?EITEMS[ii]:mi==3?TITEMS[ii]:HITEMS[ii];
}
static void draw_menus(int mx,int my){
    // v14.4: по-красиво меню — градиент и оранжев акцент
    int menu_w = real_w>0?real_w:WIN_W;
    for(int i=0;i<MENU_H;i++){
        int c = 10 + i*10/MENU_H;
        SDL_SetRenderDrawColor(ren,c,c,c+4,255);
        SDL_RenderDrawLine(ren,0,i,menu_w,i);
    }
    SDL_SetRenderDrawColor(ren,255,165,0,255); SDL_RenderDrawLine(ren,0,MENU_H-1,menu_w,MENU_H-1);
    SDL_SetRenderDrawColor(ren,60,60,70,255); SDL_RenderDrawLine(ren,0,MENU_H-2,menu_w,MENU_H-2);
    if(!ai_thinking){int tw=(int)(strlen(msg)*RAW_ADV);if(tw<WIN_W-200)dtxt_raw(WIN_W-tw-4,14,msg,1,130,130,160);}
    int mw=120,mx0=4,gap=4;
    for(int mi=0;mi<N_MENUS;mi++){
        int bx=mx0+mi*(mw+gap),by=3,bh=MENU_H-6;
        int hov=(mx>=bx&&mx<=bx+mw&&my>=by&&my<=by+bh),act=(open_menu==mi);
        frect(bx,by,mw,bh, act?48: (hov?28:0), act?26:(hov?28:0), act?0:(hov?28:0)); /* CMD: orange when active, gray when hover */
        if(act) orect(bx,by,mw,bh,255,165,0);
        else if(hov) orect(bx,by,mw,bh,90,90,90);
        int tw=(int)(strlen(MNAME[mi])*RAW_ADV+RAW_ADV);
        dtxt_raw(bx+(mw-tw)/2,by+11,MNAME[mi],1,act?255:(hov?240:210),act?255:(hov?255:230),act?255:(hov?240:210));
        if(open_menu==mi){
            int n=menu_count(mi),ih=26,iw=(mi==5)?320:220,ix=bx,iy=MENU_H;
            if(ix+iw>WIN_W)ix=WIN_W-iw-2;
            // soft shadow
            SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren,0,0,0,90);
            SDL_Rect sh={ix+4,iy+4,iw,n*ih+8}; SDL_RenderFillRect(ren,&sh);
            SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
            frect(ix,iy,iw,n*ih+8,18,18,24);orect(ix,iy,iw,n*ih+8,80,80,95);
            SDL_SetRenderDrawColor(ren,255,165,0,120); SDL_RenderDrawLine(ren,ix,iy,ix+iw,iy);
            for(int ii=0;ii<n;ii++){
                int iy2=iy+4+ii*ih;
                /* separator rows (header rows starting with ---) */
                int is_sep = 0;
                if(mi==1 && (ii==3||ii==12||ii==20||ii==23)) is_sep=1;
                if(mi==2 && (ii==0||ii==2||ii==4)) is_sep=1;
                if(mi==3 && (ii==0||ii==6||ii==11||ii==16)) is_sep=1;
                if(is_sep){
                    SDL_SetRenderDrawColor(ren,80,70,50,255);
                    SDL_RenderDrawLine(ren,ix+8,iy2+ih/2,ix+iw-8,iy2+ih/2);
                    SDL_SetRenderDrawColor(ren,40,40,50,255);
                    SDL_RenderDrawLine(ren,ix+8,iy2+ih/2+1,ix+iw-8,iy2+ih/2+1);
                    dtxt_raw(ix+8,iy2+4,menu_item(mi,ii),1,80,80,120);
                    continue;
                }
                int hovi=(mx>=ix&&mx<ix+iw&&my>=iy2&&my<iy2+ih);
                if(hovi){ frect(ix+2,iy2,iw-4,ih-2,35,30,20); SDL_SetRenderDrawColor(ren,255,165,0,255); SDL_RenderDrawLine(ren,ix+2,iy2,ix+2,iy2+ih-2); }
                int mk=0;
                if(mi==1){
                    if(ii==0&&sound_on)mk=1; if(ii==1&&flip_board)mk=1; if(ii==2&&use_book)mk=1;
                    if(ii==4&&cur_theme==0)mk=1; if(ii==5&&cur_theme==1)mk=1; if(ii==6&&cur_theme==2)mk=1; if(ii==7&&cur_theme==3)mk=1;
                    if(ii==8&&cur_theme==7)mk=1; if(ii==9&&cur_theme==4)mk=1; if(ii==10&&cur_theme==5)mk=1; if(ii==11&&cur_theme==6)mk=1;
                    if(ii==13&&base_time==1*60*1000&&increment==0)mk=1;
                    if(ii==14&&base_time==3*60*1000&&increment==0)mk=1;
                    if(ii==15&&base_time==5*60*1000&&increment==0)mk=1;
                    if(ii==16&&base_time==10*60*1000&&increment==0)mk=1;
                    if(ii==17&&base_time==30*60*1000&&increment==0)mk=1;
                    if(ii==18&&base_time==60*60*1000&&increment==0)mk=1;
                    if(ii==19&&base_time==120*60*1000&&increment==0)mk=1;
                    if(ii==21&&base_time==5*60*1000&&increment==3*1000)mk=1;
                    if(ii==22&&base_time==4*60*1000&&increment==2*1000)mk=1;
                    if(ii==24&&use_ponder)mk=1;
                }
                if(mi==2){
                    if(ii==1&&!use_uci_engine)mk=1;
                    if(ii==3&&use_uci_engine&&uci_eng[0].ready)mk=1;
                    if(ii==5&&use_uci_engine&&uci_eng[1].ready)mk=1;
                }
                if(mi==3){
                    if(ii==1&&tourney_total==2)mk=1;
                    if(ii==2&&tourney_total==4)mk=1;
                    if(ii==3&&tourney_total==10)mk=1;
                    if(ii==4&&tourney_total==20)mk=1;
                    if(ii==5&&tourney_total!=2&&tourney_total!=4&&tourney_total!=10&&tourney_total!=20)mk=1;
                    if(ii==7&&tourney_player[0]==0)mk=1;
                    if(ii==8&&tourney_player[0]==1)mk=1;
                    if(ii==9&&tourney_player[0]==2)mk=1;
                    if(ii==10&&tourney_player[0]==3)mk=1;
                    if(ii==12&&tourney_player[1]==0)mk=1;
                    if(ii==13&&tourney_player[1]==1)mk=1;
                    if(ii==14&&tourney_player[1]==2)mk=1;
                    if(ii==15&&tourney_player[1]==3)mk=1;
                    if(ii==17&&tourney_active)mk=1;
                }
                if(mk)dtxt_raw(ix+4,iy2+8,"*",1,100,220,100);
                int txtx = ix+20;
                /* v13: show a color swatch for theme entries so each theme is
                   displayed with its own color, grouped like the others */
                if(mi==1 && ii>=4 && ii<=11){
                    int tidx = (ii==4)?0:(ii==5)?1:(ii==6)?2:(ii==7)?3:(ii==8)?7:(ii==9)?4:(ii==10)?5:6;
                    frect(ix+16, iy2+7, 12,12, THEMES[tidx].lr, THEMES[tidx].lg, THEMES[tidx].lb);
                    orect(ix+16, iy2+7, 12,12, 120,120,120);
                    txtx = ix+32;
                }
                dtxt_raw(txtx,iy2+8,menu_item(mi,ii),1,hovi?255:200,hovi?255:200,hovi?255:230);
            }
        }
    }
}

/* forward declarations */
/* v15: Tournament Manager */
static void tm_record_result(void);
static void tm_start(void);
static void tm_stop(void);
static void draw_tourney_manager(void);
static void do_undo(void);
static void do_move_full(Move *m);
static void save_pgn(void);
static void load_pgn(void);
static void pgn_replay_prev(void);
static void pgn_replay_next(void);
static void pgn_replay_first(void);
static void pgn_replay_last(void);
static void pgn_replay_auto_play(void);
static void stop_ai(void);
static void start_pondering(void);
static void start_ai_move(void);
static void start_analysis(void);
static void stop_analysis(void);

static void stop_pondering(void);
static _Atomic int ponder_running = 0;

/* v10 forward declarations */
static void uci_dbg_open(void);
static void uci_dbg_log(const char *dir, int ei, const char *line);
static void log_move_played(int idx); /* v13: forward decl — used by apply(), defined near do_move_full */
static void uci_close_engine(int ei);
static int  uci_spawn_engine(int ei, const char *path);
static int  uci_send_raw(int ei, const char *s);
static int  uci_recv_line(int ei, char *buf, int maxlen, int timeout_ms);
static void tourney_start_now(void);
static void tourney_start_round_robin(int mode);
static void tourney_stop(void);
static void tourney_record_result(void);
/* v12.7: UCI ponder (permanent brain for external engines) */
static void uci_ponder_opponent_moved(const Move *m);
static void cancel_uci_ponder(void);
static void start_uci_ponder(int ei);
static char uci_ponder_pos[8192];
static Move uci_ponder_predicted;
static _Atomic int uci_ponder_ei = -1;
static _Atomic int uci_ponder_cancel = 0;
static _Atomic int uci_ponder_best_set = 0;
static Move uci_ponder_best;
static _Atomic int uci_ponder_waiting = 0;  /* ponderhit sent; the reply is on its way */
static char uci_ponder_go[128]="go ponder"; /* v13.1: ponder go-command WITH time control (built in start_uci_ponder) */
static int uci_ponder_budget_ms=1000;       /* movetime budget mirrored into the ponder search */
/* v12.10 FIX: every UCI move unconditionally does stop + two isready
   round-trips before the real position/go (needed to guard against stale
   bestmove replies -- see uci_thread_func). That handshake takes real
   wall-clock time, and the player's clock ticks the whole time it's the
   engine's turn, so this administrative overhead was silently eating into
   the engine's (and effectively the game's) time budget on every single
   move. Heavier engines (Stockfish with big Hash/Threads) feel this far
   more than the built-in engine, which never goes through this protocol
   at all -- exactly the "delay before it starts thinking, then loses on
   time" symptom. uci_thread_func measures how long the handshake actually
   took and stores it here; apply_ai() credits it straight back to the
   mover's clock so it's never charged against the player. */
static _Atomic int last_move_overhead_ms = 0;
static _Atomic int uci_ponder_alive = 0;    /* ponder thread running */
static Move uci_last_bestmove_ponder[MAX_ENGINES]; /* predicted reply from "bestmove M ponder R" */
static int  uci_last_mover_ei = -1;         /* which engine produced the current ai_result */
static void tourney_begin_game(void);
static void start_uci_ai_move(int ei);
static void draw_fen_dialog(void);
static void draw_custom_games_dialog(void);
static void draw_path_dialog(void);

static void draw_stropt_dialog(void);
static void draw_uci_options_dialog(void);

/* v12.10: Disable the engine's own PermanentBrain when starting a game
   (engines like Sila run their own internal PB and would think on THEIR
   own outside of any go-ponder, causing double-thinking and wasted time).
   NOTE: Ponder itself must stay TRUE — a ponder engine with "Ponder=false"
   silently ignores "go ponder" and stops announcing "bestmove M ponder R",
   so the GUI's pondering can never start on the opponent's move. */
static void uci_disable_engine_pb(int ei){
    if(!uci_eng[ei].ready || !UCI_VALID(ei)) return;
    uci_send_raw(ei, "setoption name Ponder value true");
    uci_send_raw(ei, "setoption name PermanentBrain value false");
    /* v13 FIX: keep the GUI's own gate flag in sync with what was just
       forced on the engine. Without this, uci_engine_ponder_enabled()
       kept reading the engine's declared default (false for most engines
       unless the user manually ticked "Ponder" in the UCI Options dialog
       beforehand), so start_uci_ponder() silently never fired for a
       freshly loaded external engine even though the engine itself was
       told Ponder=true and would happily answer "go ponder". */
    for(int i=0;i<uci_eng[ei].num_options;i++){
        if(strcmp(uci_eng[ei].options[i].name,"Ponder")==0){
            uci_eng[ei].options[i].cur_check = 1;
            break;
        }
    }
}
static void uci_set_book(int ei, int enable){
    if(!uci_eng[ei].ready || !UCI_VALID(ei)) return;
    // Common UCI book options — engines ignore unknown ones
    uci_send_raw(ei, enable ? "setoption name OwnBook value true" : "setoption name OwnBook value false");
    uci_send_raw(ei, enable ? "setoption name UseBook value true" : "setoption name UseBook value false");
    uci_send_raw(ei, enable ? "setoption name Book value true" : "setoption name Book value false");
    uci_send_raw(ei, enable ? "setoption name Use Book value true" : "setoption name Use Book value false");
}
static void uci_set_book_for_all(int enable){
    for(int ei=0; ei<MAX_ENGINES; ei++) uci_set_book(ei, enable);
    char _bmsg[64]; snprintf(_bmsg,sizeof(_bmsg),"Book %s for UCI engines", enable?"ON":"OFF");
    uci_dbg_log("BOOK", -1, _bmsg);
    bottom_log_push(_bmsg);
}

/* v12.11: Check if a UCI engine has Ponder enabled in its options.
   The GUI's go-ponder should only be sent when the engine wants it. */
static int uci_engine_ponder_enabled(int ei){
    if(ei<0||ei>=MAX_ENGINES) return 0;
    for(int i=0;i<uci_eng[ei].num_options;i++){
        if(strcmp(uci_eng[ei].options[i].name,"Ponder")==0)
            return uci_eng[ei].options[i].cur_check;
    }
    return 1; /* default: enabled (engines that don't advertise Ponder still accept it) */
}

static void handle_menu(int mx,int my){
    int mw=120,mx0=4,gap=4;
    for(int mi=0;mi<N_MENUS;mi++){
        int bx=mx0+mi*(mw+gap);
        if(mx>=bx&&mx<=bx+mw&&my>=3&&my<=MENU_H-3){open_menu=(open_menu==mi)?-1:mi;menu_outside_t0=0;return;}
    }
    if(open_menu>=0){
        int mi=open_menu,n=menu_count(mi),ih=26,iw=(mi==5)?320:220,ix=mx0+mi*(mw+gap),iy=MENU_H;
        if(ix+iw>WIN_W)ix=WIN_W-iw-2;
        for(int ii=0;ii<n;ii++){
            int iy2=iy+4+ii*ih;
            /* skip separator/header rows */
            int is_sep=0;
            if(mi==1&&(ii==3||ii==12||ii==20||ii==23))is_sep=1;
            if(mi==2&&(ii==0||ii==2||ii==4))is_sep=1;
            if(mi==3&&(ii==0||ii==6||ii==11||ii==16))is_sep=1;
            if(is_sep) continue;
            if(mx>=ix&&mx<ix+iw&&my>=iy2&&my<iy2+ih){
                open_menu=-1;
                if(mi==0){
                    if(ii==0){stop_analysis();stop_pondering();stop_ai();pgn_replay_mode=0;pgn_replay_auto=0;player_color=WHITE;flip_board=0;aivsai=0;both_human=0;tourney_active=0;tourney_waiting=0;init_board();turn=WHITE;game_start_fen[0]=0;
                        for(int _ei=0;_ei<MAX_ENGINES;_ei++){uci_send_raw(_ei,"ucinewgame");uci_disable_engine_pb(_ei);}
                        strcpy(msg,"Your move (White)");}
                    else if(ii==1){stop_analysis();stop_pondering();stop_ai();pgn_replay_mode=0;pgn_replay_auto=0;player_color=BLACK;flip_board=0;aivsai=0;both_human=0;tourney_active=0;tourney_waiting=0;init_board();turn=WHITE;game_start_fen[0]=0;
                        for(int _ei=0;_ei<MAX_ENGINES;_ei++){uci_send_raw(_ei,"ucinewgame");uci_disable_engine_pb(_ei);}
                        strcpy(msg,"Computer thinking...");}
                    else if(ii==2){stop_analysis();stop_pondering();stop_ai();pgn_replay_mode=0;pgn_replay_auto=0;player_color=WHITE;aivsai=1;both_human=0;tourney_active=0;tourney_waiting=0;init_board();turn=WHITE;game_start_fen[0]=0;
                        for(int _ei=0;_ei<MAX_ENGINES;_ei++){uci_send_raw(_ei,"ucinewgame");uci_disable_engine_pb(_ei);}
                        strcpy(msg,"AI vs AI");}
                    else if(ii==3){open_menu=-1;fen_dialog_active=1;fen_dialog_buf[0]=0;fen_dialog_len=0;SDL_StartTextInput();}
                    else if(ii==4) load_pgn();
                    else if(ii==5){
                        char *clip = SDL_GetClipboardText();
                        if(clip && strlen(clip)>10){
                            char tmp[256]; strncpy(tmp,clip,255); tmp[255]=0;
                            SDL_free(clip);
                            stop_analysis();stop_pondering();stop_ai();pgn_replay_mode=0;pgn_replay_auto=0;
                            if(parse_fen(tmp)){
                                strncpy(game_start_fen,tmp,255);
                                hist_n=0;game_hist_n=0;
                                if(game_hist_n<MAX_GAME_HIST)game_hist_hashes[game_hist_n++]=B.hash;
                                clk_w=clk_b=base_time;last_ms=SDL_GetTicks();clock_started=0;
                                lm_fr=lm_fc=lm_tr=lm_tc=-1;
                                promo_pending=0;memset(cap_w,0,sizeof cap_w);memset(cap_b,0,sizeof cap_b);
                                sel_r=sel_c=-1;game_over=0;draw_offered=0;
                                strcpy(msg,"FEN pasted");
                            } else strcpy(msg,"Invalid FEN in clipboard!");
                        } else { if(clip) SDL_free(clip); strcpy(msg,"Clipboard empty!"); }
                    }
                    else if(ii==6){stop_pondering();if(!ai_thinking){do_undo();if(!both_human && hist_n>0&&turn!=player_color)do_undo();}}
                    else if(ii==7){draw_offered=1;sprintf(msg,"Draw offered");SDL_SetWindowTitle(win,msg);}
                    else if(ii==8){game_over=1;strcpy(msg,player_color==WHITE?"You resign":"Computer wins");stop_analysis();stop_pondering();stop_ai();}
                    else if(ii==9) save_pgn();
                    else if(ii==10) pgn_replay_prev();
                    else if(ii==11) pgn_replay_next();
                    else if(ii==12) pgn_replay_first();
                    else if(ii==13) pgn_replay_last();
                    else if(ii==14) pgn_replay_auto_play();
                }
                if(mi==1){
                    if(ii==0)sound_on=!sound_on;
                    else if(ii==1)flip_board=!flip_board;
                    else if(ii==2){ use_book=!use_book; uci_set_book_for_all(use_book); if(use_book) bottom_log_push("Book ON — GUI book + UCI OwnBook ON"); else bottom_log_push("Book OFF — GUI book + UCI OwnBook OFF"); }
                    else if(ii==4){cur_theme=0;}
                    else if(ii==5){cur_theme=1;}
                    else if(ii==6){cur_theme=2;}
                    else if(ii==7){cur_theme=3;}
                    else if(ii==8){cur_theme=7;}
                    else if(ii==9){cur_theme=4;}
                    else if(ii==10){cur_theme=5;}
                    else if(ii==11){cur_theme=6;}
                    else if(ii==13){base_time=1*60*1000;increment=0;}
                    else if(ii==14){base_time=3*60*1000;increment=0;}
                    else if(ii==15){base_time=5*60*1000;increment=0;}
                    else if(ii==16){base_time=10*60*1000;increment=0;}
                    else if(ii==17){base_time=30*60*1000;increment=0;}
                    else if(ii==18){base_time=60*60*1000;increment=0;}
                    else if(ii==19){base_time=120*60*1000;increment=0;}
                    else if(ii==21){base_time=5*60*1000;increment=3*1000;}
                    else if(ii==22){base_time=4*60*1000;increment=2*1000;}
                    else if(ii==24){ use_ponder=!use_ponder; if(!use_ponder){ stop_pondering(); cancel_uci_ponder(); bottom_log_push("Ponder OFF"); } else bottom_log_push("Ponder ON"); }
                    if(ii>=13&&ii<=22){ char _dbg[96]; snprintf(_dbg,sizeof _dbg,
                        "Settings menu ii=%d clicked -> base_time=%u increment=%u",
                        ii, base_time, increment); uci_dbg_log("CLOCK", -1, _dbg); }
                }
                if(mi==2){
                    if(ii==1){
                        use_uci_engine=0; active_engine=0;
                        strcpy(msg,"Using built-in StrongEngine");
                    }
                    else if(ii==3){
                        open_menu=-1; path_dialog_engine_idx=0;
#ifdef _WIN32
                        OPENFILENAMEA ofn={0};
                        char szFile[260]="";
                        ofn.lStructSize=sizeof(ofn);
                        ofn.lpstrFile=szFile;
                        ofn.nMaxFile=sizeof(szFile);
                        ofn.lpstrFilter="Chess engine\0*.exe\0All files\0*.*\0";
                        ofn.nFilterIndex=1;
                        ofn.lpstrTitle="Select UCI Engine 1";
                        ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
                        if(GetOpenFileNameA(&ofn)){
                            strncpy(uci_eng[0].path,szFile,255);
                            if(eng_analysis[0].is_thinking) stop_ai();
                            stop_analysis(); stop_pondering();
                            uci_close_engine(0);
                            if(uci_spawn_engine(0, uci_eng[0].path)){
                                uci_eng[0].ready=1; use_uci_engine=1; active_engine=0;
                                char *nm=strrchr(uci_eng[0].path,'\\');
                                sprintf(msg,"UCI Engine 1: %s",nm?nm+1:uci_eng[0].path);
                            } else {
                                uci_eng[0].ready=0;
                                sprintf(msg,"UCI Engine 1 failed: %s",uci_eng[0].path);
                            }
                        }
#else
                        path_dialog_active=1;
                        strncpy(path_dialog_buf,uci_eng[0].path,259);
                        path_dialog_len=strlen(path_dialog_buf);
                        SDL_StartTextInput();
#endif
                    }
                    else if(ii==5){
                        open_menu=-1; path_dialog_engine_idx=1;
#ifdef _WIN32
                        OPENFILENAMEA ofn={0};
                        char szFile[260]="";
                        ofn.lStructSize=sizeof(ofn);
                        ofn.lpstrFile=szFile;
                        ofn.nMaxFile=sizeof(szFile);
                        ofn.lpstrFilter="Chess engine\0*.exe\0All files\0*.*\0";
                        ofn.nFilterIndex=1;
                        ofn.lpstrTitle="Select UCI Engine 2";
                        ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
                        if(GetOpenFileNameA(&ofn)){
                            strncpy(uci_eng[1].path,szFile,255);
                            if(eng_analysis[1].is_thinking) stop_ai();
                            stop_analysis(); stop_pondering();
                            uci_close_engine(1);
                            if(uci_spawn_engine(1, uci_eng[1].path)){
                                uci_eng[1].ready=1; use_uci_engine=1; active_engine=1;
                                char *nm=strrchr(uci_eng[1].path,'\\');
                                sprintf(msg,"UCI Engine 2: %s",nm?nm+1:uci_eng[1].path);
                            } else {
                                uci_eng[1].ready=0;
                                sprintf(msg,"UCI Engine 2 failed: %s",uci_eng[1].path);
                            }
                        }
#else
                        path_dialog_active=1;
                        strncpy(path_dialog_buf,uci_eng[1].path,259);
                        path_dialog_len=strlen(path_dialog_buf);
                        SDL_StartTextInput();
#endif
                    }
                }
                if(mi==3){
                    /* Tournament menu */
                    if(ii==1)tourney_total=2;
                    else if(ii==2)tourney_total=4;
                    else if(ii==3)tourney_total=10;
                    else if(ii==4)tourney_total=20;
                    else if(ii==5){custom_games_dialog_active=1;custom_games_buf[0]=0;custom_games_len=0;SDL_StartTextInput();open_menu=-1;}
                    else if(ii==7)tourney_player[0]=0;
                    else if(ii==8)tourney_player[0]=1;
                    else if(ii==9)tourney_player[0]=2;
                    else if(ii==10)tourney_player[0]=3;
                    else if(ii==12)tourney_player[1]=0;
                    else if(ii==13)tourney_player[1]=1;
                    else if(ii==14)tourney_player[1]=2;
                    else if(ii==15)tourney_player[1]=3;
                    else if(ii==17)tourney_start_now();
                    /* v18 FIX: the dropdown's "Stop tournament" called tourney_stop()
                       directly, which only clears the legacy tourney_active flag.
                       If a Tournament Manager run was in progress, tm_active stayed
                       1 — the game/engines did stop, but reopening the Manager still
                       showed "running" (STOP TOURNAMENT) because tm_active was never
                       cleared. Route through tm_stop() when a TM run is active so
                       both flags — and the Manager's button — agree. */
                    else if(ii==18){ if(tm_active) tm_stop(); else tourney_stop(); }
                    else if(ii==20){tourney_mgr_active=1; open_menu=-1;}
                }
                if(mi==4){
                    /* v12.3: Options menu now just launches the overlay window */
                    uci_opts_dialog_active=1;
                    options_engine = ii; /* ii==0 -> Engine 1, ii==1 -> Engine 2 */
                    options_scroll = 0;
                    open_menu=-1;
                    return;
                }
                return;
            }
        }
        /* v13.1: click missed every item — close the menu (with the motion
           grace above, an outside click is now the way to dismiss it). */
        open_menu=-1;
    }
}


static void move_to_string(Move *m, char *out) {
    if(m->castle==1||m->castle==3){strcpy(out,"O-O");return;}
    else if(m->castle==2||m->castle==4){strcpy(out,"O-O-O");return;}
    sprintf(out,"%c%d%c%d", 'a'+m->fc, 8-m->fr, 'a'+m->tc, 8-m->tr);
    if(m->promo){char promo[3]="="; promo[1]=" NBRQ"[m->promo]; strcat(out,promo);}
}

/* v12: captured-pieces tray + material advantage indicator
   cap_w[] = pieces White has captured (from Black), cap_b[] = pieces Black has captured (from White) */
static const int PIECE_VAL[7]={0,1,3,3,5,9,0};
static int material_diff(void){
    int d=0;
    for(int t=1;t<=5;t++) d += cap_w[t]*PIECE_VAL[t] - cap_b[t]*PIECE_VAL[t];
    return d;
}
/* v13: render a side's captured pieces inside a box (x,y,w,h). The piece icons
   are scaled to the box and wrap onto a new row when they reach the right edge,
   so the layout is always proportional to the panel and never spills out. */
static void draw_captured_box(int x,int y,int w,int h,int white_side,const char*label){
    dtxt(x,y,label,1,170,170,170);
    int *cap = white_side ? cap_w : cap_b;
    int texcol = white_side ? 0 : 1; /* colour of the captured pieces themselves */
    int gx=x, gy=y+UI_LINE_H+2, gh=h-(UI_LINE_H+2);
    int total=0; for(int t=1;t<=5;t++) total+=cap[t];
    if(total==0){ dtxt(x, gy+ (gh>20?6:0), "(none)",1,90,90,90); return; }
    /* pick an icon size that fits the box both horizontally (wrap) and vertically */
    int cs=26;
    int cols = w/cs; if(cols<1)cols=1;
    int rows = (total+cols-1)/cols;
    while(rows*cs > gh && cs>10){ cs-=2; cols = w/cs; if(cols<1)cols=1; rows=(total+cols-1)/cols; }
    int grid_w=cols*cs;
    int sx=gx+(w-grid_w)/2, sy=gy+(gh-rows*cs)/2;
    int idx=0;
    for(int t=5;t>=1;t--){
        for(int n=0;n<cap[t];n++){
            int col=idx%cols, row=idx/cols;
            int px=sx+col*cs, py=sy+row*cs, icon=cs-3;
            /* v12.2: light backing chip so black-piece icons don't vanish */
            frect(px-1,py-1,icon+2,icon+2,150,150,160);
            if(tex[texcol][t]){
                SDL_Rect d={px,py,icon,icon};
                SDL_RenderCopy(ren,tex[texcol][t],NULL,&d);
            } else {
                fcircle(px+icon/2,py+icon/2,icon/2-1, texcol?230:60,texcol?230:60,texcol?210:55);
            }
            idx++;
        }
    }
    int md=material_diff();
    if((white_side&&md>0)||(!white_side&&md<0)){
        char b[8]; snprintf(b,sizeof b,"+%d", white_side?md:-md);
        dtxt(x+w-(int)(strlen(b)*UI_ADV), y, b,1,225,185,90);
    }
}
/* v12: small eval-history sparkline, drawn next to the material eval bar */
static void draw_eval_sparkline(int x,int y,int w,int h){
    // v14.2: улучшена графика — повече информация, по-красива
    frect(x,y,w,h,0,0,0); orect(x,y,w,h,70,70,70);
    // хоризонтална мрежа
    SDL_SetRenderDrawColor(ren,28,28,28,255);
    for(int gy=1;gy<4;gy++){ int yy=y+h*gy/4; SDL_RenderDrawLine(ren,x,yy,x+w,yy); }
    SDL_SetRenderDrawColor(ren,60,60,60,255);
    SDL_RenderDrawLine(ren,x,y+h/2,x+w,y+h/2);
    // Y етикети: +10, +5, 0, -5, -10 (центърът е 0)
    dtxt_raw(x+w+4, y-4, "+10",1, 100,100,100);
    dtxt_raw(x+w+4, y+h*25/100-4, "+5",1, 100,100,100);
    dtxt_raw(x+w+4, y+h/2-4, "0",1, 140,140,140);
    dtxt_raw(x+w+4, y+h*75/100-4, "-5",1, 100,100,100);
    dtxt_raw(x+w+4, y+h-8, "-10",1, 100,100,100);
    int n=hist_n; if(n<1||w<20) return;
    int start=n>60?n-60:0;
    int cnt=n-start;
    int evfl = flip_board ^ (player_color==BLACK ? 1 : 0);
    // вертикални тикове през 5 хода + номера на ходовете през 10
    SDL_SetRenderDrawColor(ren,35,35,35,255);
    for(int i=start;i<n;i++) if((i-start)%5==0){
        int px=x+(int)((float)(i-start)/(float)(cnt>1?cnt-1:1)*w);
        SDL_RenderDrawLine(ren,px,y,px,y+h);
        if((i-start)%10==0 && i>=start){
            char lbl[8]; snprintf(lbl,sizeof(lbl),"%d", i/2+1);
            // само за бели ходове показваме номер, за да не се пренасища
            if(i%2==0) dtxt_raw(px-6, y+h-8, lbl,1, 70,70,70);
        }
    }
    // събираме точки за запълване и статистика
    int pts_x[64], pts_y[64], pts_ev[64];
    int pcnt=0;
    int min_ev=10000, max_ev=-10000, sum_ev=0;
    for(int i=start;i<n && pcnt<64;i++){
        int ev=(i<EVAL_HIST_MAX)?eval_hist[i]:0;
        if(evfl) ev=-ev;
        if(ev>1500) ev=1500; if(ev<-1500) ev=-1500;
        if(ev<min_ev) min_ev=ev;
        if(ev>max_ev) max_ev=ev;
        sum_ev+=ev;
        float frac=0.5f - ev/3000.0f;
        if(frac<0) frac=0; if(frac>1) frac=1;
        int px=x+(int)((float)(i-start)/(float)(cnt>1?cnt-1:1)*w);
        int py=y+(int)(frac*h);
        if(py<y) py=y; if(py>y+h) py=y+h;
        pts_x[pcnt]=px; pts_y[pcnt]=py; pts_ev[pcnt]=ev; pcnt++;
    }
    if(pcnt<1) { orect(x,y,w,h,70,70,70); return; }
    // леко запълване — по-дискретно, за да не се слива с бара
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,0,0,0,0);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    // линия на графиката — по-дебела и с цвят според оценката
    int prevx=pts_x[0], prevy=pts_y[0], have=1;
    for(int i=1;i<pcnt;i++){
        int px=pts_x[i], py=pts_y[i], ev=pts_ev[i];
        int is_pos = ev>30;
        int is_neg = ev<-30;
        if(is_pos) SDL_SetRenderDrawColor(ren,255,165,0,255);
        else if(is_neg) SDL_SetRenderDrawColor(ren,80,130,220,255);
        else SDL_SetRenderDrawColor(ren,180,180,180,255);
        SDL_RenderDrawLine(ren,prevx,prevy,px,py);
        SDL_RenderDrawLine(ren,prevx,prevy+1,px,py+1);
        prevx=px; prevy=py;
    }
    // точки за всеки ход
    for(int i=0;i<pcnt;i++){
        int px=pts_x[i], py=pts_y[i], ev=pts_ev[i];
        SDL_SetRenderDrawColor(ren, ev>30?255: (ev<-30?80:180), ev>30?165: (ev<-30?130:180), ev>30?0: (ev<-30?220:180),255);
        SDL_Rect d={px-2,py-2,4,4}; SDL_RenderFillRect(ren,&d);
        SDL_SetRenderDrawColor(ren,0,0,0,255); SDL_RenderDrawRect(ren,&d);
    }
    // без дублирана оценка — графиката показва само линията
    orect(x,y,w,h,70,70,70);
}
/* v12.1: lightweight opening-name recognizer (coordinate-notation prefix match).
   This is a hand-picked set of well-known lines, not a full ECO database — a real
   ECO table has 500+ entries and isn't something we have bundled/embeddable here.
   Coverage is now deep enough that engine games (which love flank/odd first moves)
   should almost always land on at least a named "Opening" or family, and if truly
   nothing matches we now fall back to a plain "1.<move>" label instead of blank. */
typedef struct { const char *moves; const char *name; } OpeningDef;
static const OpeningDef OPENINGS[]={
    /* ---- Ruy Lopez ---- */
    {"e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1","Ruy Lopez: Closed"},
    {"e2e4 e7e5 g1f3 b8c6 f1b5 g8f6","Ruy Lopez: Berlin Defense"},
    {"e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5c6","Ruy Lopez: Exchange Variation"},
    {"e2e4 e7e5 g1f3 b8c6 f1b5 f7f5","Ruy Lopez: Schliemann Defense"},
    {"e2e4 e7e5 g1f3 b8c6 f1b5 g7g6","Ruy Lopez: Fianchetto Defense"},
    {"e2e4 e7e5 g1f3 b8c6 f1b5","Ruy Lopez"},
    /* ---- Italian / Two Knights ---- */
    {"e2e4 e7e5 g1f3 b8c6 f1c4 f8c5","Italian Game: Giuoco Piano"},
    {"e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 d2d3","Italian Game: Giuoco Pianissimo"},
    {"e2e4 e7e5 g1f3 b8c6 f1c4 g8f6","Italian Game: Two Knights Defense"},
    {"e2e4 e7e5 g1f3 b8c6 f1c4","Italian Game"},
    {"e2e4 e7e5 g1f3 b8c6 d2d4 e5d4 f3d4","Scotch Game"},
    {"e2e4 e7e5 g1f3 b8c6 d2d4","Scotch Game"},
    {"e2e4 e7e5 g1f3 g8f6 f3e5 d7d6","Petrov's Defense"},
    {"e2e4 e7e5 g1f3 g8f6","Petrov's Defense"},
    {"e2e4 e7e5 f2f4 e5f4","King's Gambit Accepted"},
    {"e2e4 e7e5 f2f4","King's Gambit"},
    {"e2e4 e7e5 b1c3 g8f6","Vienna Game"},
    {"e2e4 e7e5","Open Game"},
    /* ---- Sicilian ---- */
    {"e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6","Sicilian Defense: Najdorf Variation"},
    {"e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 g7g6","Sicilian Defense: Dragon Variation"},
    {"e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 e7e6","Sicilian Defense: Scheveningen Variation"},
    {"e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g8f6 b1c3 e7e5","Sicilian Defense: Sveshnikov Variation"},
    {"e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g6","Sicilian Defense: Accelerated Dragon"},
    {"e2e4 c7c5 g1f3 e7e6 d2d4 c5d4 f3d4 b8c6","Sicilian Defense: Taimanov Variation"},
    {"e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3","Sicilian Defense: Open"},
    {"e2e4 c7c5 b1c3 b8c6 g2g3","Sicilian Defense: Closed Variation"},
    {"e2e4 c7c5 b1c3","Sicilian Defense: Closed Variation"},
    {"e2e4 c7c5 g1f3 d7d6","Sicilian Defense"},
    {"e2e4 c7c5 c2c3","Sicilian Defense: Alapin Variation"},
    {"e2e4 c7c5","Sicilian Defense"},
    /* ---- French ---- */
    {"e2e4 e7e6 d2d4 d7d5 e4e5","French Defense: Advance Variation"},
    {"e2e4 e7e6 d2d4 d7d5 b1c3 f8b4","French Defense: Winawer Variation"},
    {"e2e4 e7e6 d2d4 d7d5 b1d2","French Defense: Tarrasch Variation"},
    {"e2e4 e7e6 d2d4 d7d5 b1c3 g8f6","French Defense: Classical Variation"},
    {"e2e4 e7e6 d2d4 d7d5 e4d5 e6d5","French Defense: Exchange Variation"},
    {"e2e4 e7e6 d2d4 d7d5","French Defense"},
    {"e2e4 e7e6","French Defense"},
    /* ---- Caro-Kann ---- */
    {"e2e4 c7c6 d2d4 d7d5 e4e5","Caro-Kann Defense: Advance Variation"},
    {"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5","Caro-Kann Defense: Classical Variation"},
    {"e2e4 c7c6 d2d4 d7d5 e4d5 c6d5","Caro-Kann Defense: Exchange Variation"},
    {"e2e4 c7c6 d2d4 d7d5 b1d2","Caro-Kann Defense: Two Knights/Tarrasch"},
    {"e2e4 c7c6 d2d4 d7d5","Caro-Kann Defense"},
    {"e2e4 c7c6","Caro-Kann Defense"},
    /* ---- Other 1.e4 replies ---- */
    {"e2e4 d7d5 e4d5 d8d5 b1c3","Scandinavian Defense: Main Line"},
    {"e2e4 d7d5","Scandinavian Defense"},
    {"e2e4 d7d6 d2d4 g8f6 b1c3 g7g6","Pirc Defense"},
    {"e2e4 d7d6","Pirc Defense"},
    {"e2e4 g7g6","Modern Defense"},
    {"e2e4 g8f6 e4e5 f6d5","Alekhine's Defense"},
    {"e2e4 g8f6","Alekhine's Defense"},
    {"e2e4","King's Pawn Game"},
    /* ---- Queen's Gambit / Slav ---- */
    {"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6","Queen's Gambit Declined: Orthodox Variation"},
    {"d2d4 d7d5 c2c4 e7e6 c4d5 e6d5","Queen's Gambit Declined: Exchange Variation"},
    {"d2d4 d7d5 c2c4 e7e6","Queen's Gambit Declined"},
    {"d2d4 d7d5 c2c4 c7c6 c4d5 c6d5","Slav Defense: Exchange Variation"},
    {"d2d4 d7d5 c2c4 c7c6","Slav Defense"},
    {"d2d4 d7d5 c2c4 d5c4","Queen's Gambit Accepted"},
    {"d2d4 d7d5 c2c4","Queen's Gambit"},
    {"d2d4 d7d5","Queen's Pawn Game"},
    /* ---- Indian systems ---- */
    {"d2d4 g8f6 c2c4 e7e6 b1c3 f8b4 e2e3","Nimzo-Indian Defense: Rubinstein Variation"},
    {"d2d4 g8f6 c2c4 e7e6 b1c3 f8b4 d1c2","Nimzo-Indian Defense: Classical Variation"},
    {"d2d4 g8f6 c2c4 e7e6 b1c3 f8b4","Nimzo-Indian Defense"},
    {"d2d4 g8f6 c2c4 e7e6 g1f3 b7b6","Queen's Indian Defense"},
    {"d2d4 g8f6 c2c4 e7e6","Queen's Indian setup"},
    {"d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6 g1f3 e8g8","King's Indian Defense: Classical Variation"},
    {"d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6 f2f3","King's Indian Defense: Saemisch Variation"},
    {"d2d4 g8f6 c2c4 g7g6 b1c3 d7d5 c4d5 f6d5 e2e4","Grunfeld Defense: Exchange Variation"},
    {"d2d4 g8f6 c2c4 g7g6 b1c3 d7d5","Grunfeld Defense"},
    {"d2d4 g8f6 c2c4 g7g6","King's Indian Defense"},
    {"d2d4 g8f6 c2c4 c7c5 d4d5 e7e6","Benoni Defense"},
    {"d2d4 g8f6 c2c4 c7c5","Benoni Defense"},
    {"d2d4 g8f6 g1f3 g7g6","Indian Defense: Kingside Fianchetto"},
    {"d2d4 g8f6","Indian Defense"},
    {"d2d4 f7f5","Dutch Defense"},
    {"d2d4","Queen's Pawn Game"},
    /* ---- Flank openings ---- */
    {"c2c4 e7e5","English Opening: Reversed Sicilian"},
    {"c2c4 c7c5","English Opening: Symmetrical Variation"},
    {"c2c4 g8f6","English Opening: Anglo-Indian"},
    {"c2c4","English Opening"},
    {"g1f3 d7d5 c2c4","Reti Opening"},
    {"g1f3 g8f6 c2c4","Reti Opening"},
    {"g1f3","Reti Opening"},
    {"g2g3","King's Fianchetto Opening"},
    {"b2b3","Nimzo-Larsen Attack"},
    {"b1c3","Van Geet Opening"},
    {"c2c3","Saragossa Opening"},
    {"e2e3","Van't Kruijs Opening"},
    {"f2f4","Bird's Opening"},
    {NULL,NULL}
};
/* v12.1: minimal move->text label (not full SAN — no check/mate marks, no
   disambiguation) used only as a last-resort fallback so something always shows */
static void move_lite_label(Move *m, BBoard *before, char *out){
    if(m->castle==1||m->castle==3){strcpy(out,"O-O");return;}
    if(m->castle==2||m->castle==4){strcpy(out,"O-O-O");return;}
    int p=bb_piece_at_rc(before,m->fr,m->fc);int tp=abs(p);
    static const char*PCL[7]={"","","N","B","R","Q","K"};
    char dest[4];sprintf(dest,"%c%d",'a'+m->tc,8-m->tr);
    int is_cap=m->cap||m->ep_cap>=0;
    if(tp==PAWN){
        if(is_cap) sprintf(out,"%cx%s",'a'+m->fc,dest); else strcpy(out,dest);
        if(m->promo){char pb[4];sprintf(pb,"=%s",PCL[m->promo]);strcat(out,pb);}
    } else sprintf(out,"%s%s%s",PCL[tp],is_cap?"x":"",dest);
}
static const char* current_opening_name(void){
    if(hist_n==0) return "";
    char played[768]=""; int n=hist_n<16?hist_n:16;
    for(int i=0;i<n;i++){
        Move *m=&hist[i].m; char mv[8];
        sprintf(mv,"%c%d%c%d",'a'+m->fc,8-m->fr,'a'+m->tc,8-m->tr);
        strcat(played,mv); if(i<n-1) strcat(played," ");
    }
    const char *best=NULL; int best_len=-1;
    for(int i=0;OPENINGS[i].moves;i++){
        int len=(int)strlen(OPENINGS[i].moves);
        if((int)strlen(played)<len) continue;
        if(strncmp(played,OPENINGS[i].moves,len)==0 && (played[len]==' '||played[len]=='\0')){
            if(len>best_len){best_len=len;best=OPENINGS[i].name;}
        }
    }
    if(best) return best;
    /* nothing named matched (rare, but engines love odd first moves) —
       show a plain move label instead of leaving the panel blank */
    static char out[24];
    char lab[8]; move_lite_label(&hist[0].m,&hist[0].bb,lab);
    snprintf(out,sizeof(out),"1.%s",lab);
    return out;
}
/* v13: Retro overlays — scanlines + vignette + CRT curvature fake */
static void draw_retro_overlays(void){
    int rw=real_w>0?real_w:WIN_W, rh=real_h>0?real_h:WIN_H;
    if(retro_scanlines){
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren,0,0,0,22);
        for(int y=MENU_H; y<rh; y+=4){
            SDL_RenderDrawLine(ren,0,y,rw,y);
        }
        /* subtle amber phosphor tint for retro themes */
        if(cur_theme==4 || cur_theme==5){
            SDL_SetRenderDrawColor(ren, retro_scanlines? 255:0, 210, 80, 6);
            SDL_Rect ov={0,MENU_H,rw,rh-MENU_H};
            SDL_RenderFillRect(ren,&ov);
        }
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    }
    if(retro_vignette){
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        for(int i=0;i<28;i++){
            int alpha = (28 - i) * 4; if(alpha>90) alpha=90;
            SDL_SetRenderDrawColor(ren,0,0,0,alpha);
            SDL_Rect top={0,MENU_H+i,rw,1};
            SDL_Rect bot={0,rh-1-i,rw,1};
            SDL_Rect lef={i, MENU_H,1,rh-MENU_H};
            SDL_Rect rig={rw-1-i, MENU_H,1,rh-MENU_H};
            SDL_RenderFillRect(ren,&top);
            SDL_RenderFillRect(ren,&bot);
            SDL_RenderFillRect(ren,&lef);
            SDL_RenderFillRect(ren,&rig);
        }
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    }
    if(retro_crt_curve){
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        /* fake curvature: darken corners more and add central brightness */
        SDL_SetRenderDrawColor(ren,0,0,0,35);
        int cx=rw/2, cy=(MENU_H+rh)/2;
        int maxd = (rw>rh?rw:rh)/2;
        for(int y=MENU_H; y<rh; y++){
            for(int x=0; x<rw; x+=32){ /* sparse sampling for perf */ }
        }
        /* border glow */
        SDL_SetRenderDrawColor(ren, 60, 50, 30, 18);
        SDL_Rect glow={BOARD_OX-6, BOARD_OY-6, BRD+12, BRD+12};
        SDL_RenderDrawRect(ren,&glow);
        SDL_Rect glow2={BOARD_OX-8, BOARD_OY-8, BRD+16, BRD+16};
        SDL_RenderDrawRect(ren,&glow2);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
        /* subtle chromatic aberration hint around frame */
        SDL_SetRenderDrawColor(ren,255,80,80,10);
        SDL_RenderDrawLine(ren, BOARD_OX-2, BOARD_OY, BOARD_OX-2, BOARD_OY+BRD);
        SDL_SetRenderDrawColor(ren,80,80,255,10);
        SDL_RenderDrawLine(ren, BOARD_OX+BRD+2, BOARD_OY, BOARD_OX+BRD+2, BOARD_OY+BRD);
    }
}

/* v14: helper to draw EVAL panel (extracted for interleaving between engines) */
static void draw_eval_panel_v14(int sy, int sw, int dyn_panel_h){
    // Uniform EVAL panel — fixed overlap, fills width
    frect(FRAME_W+4,sy,sw-8,dyn_panel_h,0,0,0);
    orect(FRAME_W+4,sy,sw-8,dyn_panel_h,70,70,70);
    dtxt(FRAME_W+9,sy+5,"EVAL",1,0,0,0);
    dtxt(FRAME_W+8,sy+4,"EVAL",1,255,165,0);
    {
        char di[256]; char engname[256];
        int show_uci = use_uci_engine; int show_ei=active_engine;
        if(tourney_active||aivsai){ int tp=(turn==WHITE)?tourney_player[0]:tourney_player[1]; if(tp==1){show_uci=1;show_ei=0;} else if(tp==2){show_uci=1;show_ei=1;} else show_uci=0; }
        const char *src = show_uci ? uci_eng[show_ei].path : "StrongEngine";
        strncpy(engname, src, 255); engname[255]=0;
        char *sl=strrchr(engname,
#ifdef _WIN32
        '\\'
#else
        '/'
#endif
        ); if(sl) memmove(engname,sl+1,strlen(sl));
        snprintf(di,sizeof(di),"%s",engname);
        // show full name if space — truncate only if extremely long
        int maxch=(int)((sw-60)/UI_ADV); if(maxch<8)maxch=8;
        if((int)strlen(di)>maxch+20){di[maxch+20-3]=0;strcat(di,"...");}
        int ecol=(show_uci&&uci_eng[show_ei].ready)?220:160;
        dtxt(FRAME_W+8+50,sy+4,di,1,ecol,ecol,ecol);
        int is_ponder_on=use_ponder; int is_th=ai_thinking||ponder_running||uci_ponder_alive||uci_ponder_waiting;
        if(is_ponder_on&&is_th){ int blink=(SDL_GetTicks()/400)%2; if(blink) fcircle(FRAME_W+4+sw-26,sy+8,4,255,165,0); else fcircle(FRAME_W+4+sw-26,sy+8,4,60,40,0); }
        else fcircle(FRAME_W+4+sw-26,sy+8,3,70,70,70);
    }
    int evfl = flip_board ^ (player_color==BLACK ? 1 : 0);
    int disp_ev = evfl ? -g_best_eval : g_best_eval;
    int ev=g_best_eval; if(ev>1500)ev=1500; if(ev<-1500)ev=-1500; if(evfl) ev=-ev;
    double frac=0.5+ev/3000.0; if(frac<0)frac=0; if(frac>1)frac=1;
    int bh=dyn_panel_h-(UI_LINE_H+5)-6, bw=28;
    int bar_x=FRAME_W+8, bar_y=sy+UI_LINE_H+5;
    {
      int bottom_h=(int)(frac*bh); int top_h=bh-bottom_h; int split=top_h;
      int bottom_is_white=!evfl;
      int tR,tG,tB,bR,bG,bB;
      if(bottom_is_white){ tR=40; tG=60; tB=95; bR=255; bG=165; bB=0; } else { tR=255; tG=165; tB=0; bR=40; bG=60; bB=95; }
      if(top_h>0) frect(bar_x,bar_y,bw,top_h,tR,tG,tB);
      if(bottom_h>0) frect(bar_x,bar_y+top_h,bw,bottom_h,bR,bG,bB);
      for(int t=1;t<4;t++){ int ty=bar_y+bh*t/4; SDL_SetRenderDrawColor(ren,0,0,0,90); SDL_RenderDrawLine(ren,bar_x,ty,bar_x+bw,ty); }
      if(split>0&&split<bh){ SDL_SetRenderDrawColor(ren,0,0,0,255); SDL_RenderDrawLine(ren,bar_x,bar_y+split,bar_x+bw,bar_y+split); SDL_SetRenderDrawColor(ren,255,255,255,180); SDL_RenderDrawLine(ren,bar_x,bar_y+split+1,bar_x+bw,bar_y+split+1); }
      orect(bar_x,bar_y,bw,bh,80,80,80);
    }
    {
        char evb[16]; if(disp_ev>9000) strcpy(evb,"M+"); else if(disp_ev<-9000) strcpy(evb,"M-"); else sprintf(evb,"%+.1f",disp_ev/100.0);
        int evcol=disp_ev>=0?255:100, evG=disp_ev>=0?165:160, evB=disp_ev>=0?0:255;
        dtxt(bar_x+bw+6, bar_y+bh/2-4, evb,1,evcol,evG,evB);
        int spx=bar_x+bw+62, spw=(sw-8) - (bw+70); if(spw<60) spw=60;
        draw_eval_sparkline(spx, bar_y, spw, bh);
    }
}

static void draw_sidebar(int mx,int my){
    int sw=real_w>FRAME_W?real_w-FRAME_W:SIDE_W;
    int sh=real_h>MENU_H?real_h-MENU_H-LOG_H:WIN_H-MENU_H-LOG_H;
    if(sh<260) sh=260;
    frect(FRAME_W,MENU_H,sw,sh,0,0,0);
    SDL_SetRenderDrawColor(ren,64,64,64,255); SDL_RenderDrawRect(ren,&(SDL_Rect){FRAME_W,MENU_H,sw,sh});
    // compute uniform panel height so ALL sidebar panels are equal size and
    // together fill the full available sidebar height (request: panels should
    // maximally use the screen and, where possible, be the same size).
    int num_eng_tmp;
    if(tourney_active || aivsai){
        /* v13: one panel per side (White/Black) so AI-vs-AI always shows two
           engine panels even when both sides use the built-in engine. A human
           side contributes no panel. */
        int pw=tourney_player[0], pb=tourney_player[1];
        num_eng_tmp = (pw!=3?1:0) + (pb!=3?1:0);
        if(num_eng_tmp<1) num_eng_tmp=1;
    } else {
        num_eng_tmp = 1; /* built-in always */
        if(uci_eng[0].ready||eng_analysis[0].has_data) num_eng_tmp++;
        if(uci_eng[1].ready||eng_analysis[1].has_data) num_eng_tmp++;
    }
    /* analysis always runs on the built-in engine -> keep its panel visible */
    if(analysis_mode){
        int hbi = (tourney_active||aivsai) ? 0 : 1;
        if(tourney_active||aivsai){
            if(tourney_player[0]!=3 && tourney_player[0]==0) hbi=1;
            if(tourney_player[1]!=3 && tourney_player[1]==0) hbi=1;
        }
        if(!hbi) num_eng_tmp++;
    }
    int has_tourney = (tourney_active||tourney_played>0)?1:0;
    int tourney_h = has_tourney ? (tourney_is_rr?(4*(UI_LINE_H+3)+8):(3*(UI_LINE_H+3)+8)) : 0;
    int num_uniform_others = 3 + num_eng_tmp; // CAPTURED + EVAL + MOVES + engines (без турнира)
    // v14: reserve space for FEN panel (fixed height) so uniform panels fill remaining
    const int FEN_H = 4 + UI_LINE_H + 2 + UI_LINE_H + 6; /* header row + fen row + padding */
    // топ маргин + ponder bar + gaps + FEN + турнирен панел (ако има)
    int avail_h = sh - 8 - 32 - num_uniform_others*6 - (has_tourney? (tourney_h+6):0) - FEN_H - 6;
    int dyn_panel_h = avail_h / (num_uniform_others>0?num_uniform_others:1);
    int num_uniform = num_uniform_others + has_tourney; // за съвместимост
    if(dyn_panel_h<90) dyn_panel_h=90;   /* CAPTURED needs ~90 for 2 rows of pieces */
    if(dyn_panel_h>220) dyn_panel_h=220; /* keep panels sane on very tall windows */
    int sy=MENU_H+8;

    /* CMD uniform: CAPTURED panel — same height as every other panel */
    int cap_h = dyn_panel_h;
    frect(FRAME_W+4, sy, sw-8, cap_h, 0,0,0);
    orect(FRAME_W+4, sy, sw-8, cap_h, 70,70,70);
    dtxt(FRAME_W+9, sy+5, "CAPTURED",1, 0,0,0);
    dtxt(FRAME_W+8, sy+4, "CAPTURED",1, 255,165,0);
    {
        const char *turn_s = turn==WHITE ? "WHITE to move" : "BLACK to move";
        // move further right to avoid overlap with CAPTURED (was 70, now 90)
        dtxt(FRAME_W+8+90, sy+4, turn_s,1, turn==WHITE?220:120, turn==WHITE?220:170, turn==WHITE?220:80);
    }
        /* v13: keep the two capture rows near the top and use the (often large)
           remaining space for live position statistics, so the panel is never
           just empty when few pieces have been captured. */
        /* v13: each side's captured pieces are laid out in a box that is scaled
           to the panel — pieces wrap to a new row at the right edge and are sized
           proportionally, so the panel is filled without spilling anywhere. */
        {
            int head=UI_LINE_H+6;
            int boxh = (cap_h-head)/2;
            if(boxh<UI_LINE_H+28) boxh=UI_LINE_H+28;
            draw_captured_box(FRAME_W+8, sy+head,        sw-16, boxh, 1, "W taken:");
            draw_captured_box(FRAME_W+8, sy+head+boxh,  sw-16, boxh, 0, "B taken:");
        }
    sy+=cap_h+6;

    /* Tournament scoreboard — компактен, за да има повече място за графиката */
    if(tourney_active||tourney_played>0){
        frect(FRAME_W+4,sy,sw-8,tourney_h,0,0,0);
        orect(FRAME_W+4,sy,sw-8,tourney_h,100,180,220);
        dtxt(FRAME_W+9,sy+7,"TOURNAMENT",1,0,0,0);
        dtxt(FRAME_W+8,sy+6,"TOURNAMENT",1,100,180,220);
        int trow = sy + 6 + (UI_LINE_H+3); /* first row below the header */
        if(tourney_is_rr){
            char ts[64];
            snprintf(ts,sizeof(ts),"RR G%d/%d", tourney_played+1, tourney_total);
            dtxt(FRAME_W+8,trow,ts,1,200,200,200);
            char line[96]=""; int off=0;
            for(int i=0;i<tourney_rr_num && off<90;i++){
                const char *nm = tourney_rr_players[i]==0?"Blt": tourney_rr_players[i]==1?"E1": tourney_rr_players[i]==2?"E2":"Hum";
                off+=snprintf(line+off,sizeof(line)-off,"%s:%.1f ", nm, tourney_rr_score[i]);
            }
            trow += UI_LINE_H+3;
            dtxt(FRAME_W+8,trow,line,1,220,180,120);
            if(tourney_waiting){
                char nxt[64]; snprintf(nxt,sizeof(nxt),"Next: %d vs %d", tourney_player[0], tourney_player[1]);
                trow += UI_LINE_H+3;
                dtxt(FRAME_W+8,trow,nxt,1,180,160,100);
            }
        } else {
            char ts[64];
            sprintf(ts,"G%d/%d  E1 %.1f  E2 %.1f",
                tourney_played+1,tourney_total,
                tourney_score[0],tourney_score[1]);
            dtxt(FRAME_W+8,trow,ts,1,200,200,200);
            trow += UI_LINE_H+3;
            if(tourney_waiting){
                dtxt(FRAME_W+8,trow,"Next game soon...",1,180,160,100);
            } else {
                dtxt(FRAME_W+8,trow,"CMD uniform panel",1,90,90,90);
            }
        }
        sy+=tourney_h+6;
    }

    /* v13: Per-engine analysis panels with names — separate for each engine */
    {
        int panel_w=sw-8; if(panel_w<180)panel_w=180;
        /* Determine which engines to show.
           v13 FIX: previously this ALWAYS showed the built-in slot (show[2]=1
           unconditionally) plus any UCI engine that happened to be ready,
           which meant a plain "Engine 1 vs Engine 2" game (or tournament)
           showed THREE panels -- the two real players plus a phantom
           built-in panel that never played a single move. Mirror the exact
           same white/black -> engine mapping that start_ai_move() uses
           (tourney_player[]/aivsai) so the panels shown always match who is
           actually assigned to play. */
        /* v13: build the engine-panel list. In AI-vs-AI / tournament mode we show
           ONE panel per SIDE (White and Black) so there are always two engine
           panels -- even when both sides use the built-in engine. A human side
           gets no panel. In normal mode we keep the old behaviour: the built-in
           panel plus whichever UCI engine(s) are loaded. */
        int panel_slot[3]; int panel_side[3]; int npan=0;
        int on_move_side = -1;
        if(tourney_active || aivsai){
            int pw=tourney_player[0], pb=tourney_player[1];
            /* tourney_player encodes 0=built-in,1=UCI1,2=UCI2,3=human, but
               eng_analysis[] slots are 0=UCI1,1=UCI2,2=built-in. */
            int spw = (pw==0)?ENG_BUILTIN_IDX:(pw==1?0:(pw==2?1:-1));
            int spb = (pb==0)?ENG_BUILTIN_IDX:(pb==1?0:(pb==2?1:-1));
            if(pw!=3 && spw>=0){ panel_slot[npan]=spw; panel_side[npan]=0; npan++; }
            if(pb!=3 && spb>=0){ panel_slot[npan]=spb; panel_side[npan]=1; npan++; }
            on_move_side = (turn==WHITE)?0:1;
        } else {
            panel_slot[npan]=ENG_BUILTIN_IDX; panel_side[npan]=-1; npan++;
            if(uci_eng[0].ready||eng_analysis[0].has_data){ panel_slot[npan]=0; panel_side[npan]=-1; npan++; }
            if(uci_eng[1].ready||eng_analysis[1].has_data){ panel_slot[npan]=1; panel_side[npan]=-1; npan++; }
        }
        /* v13 FIX: during analysis the built-in engine runs the infinite search,
           so always keep its panel on screen even when no side owns an engine
           (e.g. a human-vs-human tournament where npan would otherwise be 0). */
        if(analysis_mode){
            int has_bp=0;
            for(int _p=0;_p<npan;_p++) if(panel_slot[_p]==ENG_BUILTIN_IDX){ has_bp=1; break; }
            if(!has_bp && npan<3){ panel_slot[npan]=ENG_BUILTIN_IDX; panel_side[npan]=-1; npan++; }
        }
        int per_h = dyn_panel_h; /* uniform, fills the sidebar together with the other panels */
        int evfl = flip_board ^ (player_color==BLACK ? 1 : 0);
        int on_move_slot = -1; /* used in NORMAL mode; in aivsai/tourney on_move_side drives the highlight */
        if(tourney_active || aivsai){
            int tp = (turn==WHITE) ? tourney_player[0] : tourney_player[1];
            if(tp==0) on_move_slot = ENG_BUILTIN_IDX;
            else if(tp==1) on_move_slot = 0;
            else if(tp==2) on_move_slot = 1;
        } else {
            on_move_slot = use_uci_engine ? active_engine : ENG_BUILTIN_IDX;
        }
        for(int pi=0; pi<npan; pi++){
            int ei = panel_slot[pi];
            int side = panel_side[pi];
            /* CMD panel — black with orange when thinking, gray otherwise */
            int is_active_thinking = analysis_mode && ei==ENG_BUILTIN_IDX
                ? 1
                : ((tourney_active||aivsai)
                    ? (side>=0 && side==on_move_side)
                    : (eng_analysis[ei].is_thinking || (ai_thinking && ei==on_move_slot)));
            int is_blue = (side==1 || ei==1);
            int bgR = is_active_thinking? (is_blue? 8:16):0, bgG=is_active_thinking? (is_blue?12:12):0, bgB=is_active_thinking? (is_blue?18:0):0;
            int borR=is_active_thinking? (is_blue? 80:255):60, borG=is_active_thinking? (is_blue?130:165):60, borB=is_active_thinking? (is_blue?220:0):60;
            frect(FRAME_W+4,sy,panel_w,per_h, bgR,bgG,bgB);
            orect(FRAME_W+4,sy,panel_w,per_h, borR,borG,borB);
            /* Name */
            char ename[256];
            if(ei==ENG_BUILTIN_IDX){
                snprintf(ename,sizeof(ename),"%s", eng_analysis[ei].has_data && eng_analysis[ei].name[0] ? eng_analysis[ei].name : "StrongEngine (Built-in)");
            } else {
                /* use path basename — engines may report same "id name" (e.g. "Strong") */
                if(uci_eng[ei].path[0]){
                    const char *p = strrchr(uci_eng[ei].path,'\\'); if(!p) p=strrchr(uci_eng[ei].path,'/');
                    snprintf(ename,sizeof(ename),"%s", p ? p+1 : uci_eng[ei].path);
                } else {
                    snprintf(ename,sizeof(ename),"UCI Engine %d", ei+1);
                }
            }
            char hdr[280];
            const char *side_lbl = side==0?"White: " : side==1?"Black: " : "";
            snprintf(hdr,sizeof(hdr),"%s%s", side_lbl, ename);
            int maxch=(int)((panel_w-40)/UI_ADV); if(maxch<10) maxch=10;
            int hdr_len=(int)strlen(hdr);
            int tmp_off=0, tmp_lines=0;
            while(tmp_off<hdr_len && tmp_lines<3){
                int rem=hdr_len-tmp_off; int cop=rem>maxch?maxch:rem;
                if(rem>maxch){
                    int last_sp=-1;
                    for(int k=cop-1;k>=maxch/2;k--) if(hdr[tmp_off+k]==' '){ last_sp=k; break; }
                    if(last_sp>0) cop=last_sp;
                }
                tmp_off+=cop; while(tmp_off<hdr_len && hdr[tmp_off]==' ') tmp_off++;
                tmp_lines++;
            }
            int hdr_lines=tmp_lines; if(hdr_lines<1) hdr_lines=1;
            int hdr_extra=(hdr_lines-1)*UI_LINE_H;
            int off=0;
            for(int li=0; li<hdr_lines; li++){
                int rem=hdr_len-off; int cop=rem>maxch?maxch:rem;
                if(rem>maxch){
                    int last_sp=-1;
                    for(int k=cop-1;k>=maxch/2;k--) if(hdr[off+k]==' '){ last_sp=k; break; }
                    if(last_sp>0) cop=last_sp;
                }
                char line[120]; if(cop>119) cop=119;
                strncpy(line, hdr+off, cop); line[cop]=0;
                if(li>0){ char *p=line; while(*p==' ') p++; if(p!=line) memmove(line,p,strlen(p)+1); }
                if(li==hdr_lines-1 && off+cop < hdr_len){
                    int ll=(int)strlen(line); if(ll>=3){ line[ll-3]=0; strcat(line,"..."); }
                }
                dtxt(FRAME_W+9, sy+5+li*UI_LINE_H, line, 1, 0,0,0);
                dtxt(FRAME_W+8, sy+4+li*UI_LINE_H, line, 1, is_active_thinking? 255:200, is_active_thinking?165:200, is_active_thinking?0:200);
                off+=cop; while(off<hdr_len && hdr[off]==' ') off++;
            }
            /* thinking indicator — colored dot per side (green for White, blue
               for Black), blinking while searching, instead of the old *THINKING* text */
            {
                int dx=FRAME_W+4+panel_w-12, dy=sy+10;
                int dotR = (side==1)?70:70, dotG = (side==1)?140:200, dotB = (side==1)?255:120;
                if(is_active_thinking){
                    int blink=(SDL_GetTicks()/350)%2;
                    fcircle(dx,dy,4, blink?dotR:dotR*0.65, blink?dotG:dotG*0.65, blink?dotB:dotB*0.65);
                } else {
                    fcircle(dx,dy,3, 70,70,70);
                }
            }
            if(is_active_thinking){
                /* Stats line — само когато двигателят е на ход */
                int disp_depth = eng_analysis[ei].has_data ? eng_analysis[ei].depth : g_best_depth;
                int disp_eval_raw = eng_analysis[ei].has_data ? eng_analysis[ei].eval : g_best_eval;
                int evforside = evfl ? -disp_eval_raw : disp_eval_raw;
                char evals[24];
                if(evforside>9000) strcpy(evals,"M+");
                else if(evforside<-9000) strcpy(evals,"M-");
                else snprintf(evals,sizeof(evals),"%+.2f", evforside/100.0);
                char st1[96];
                long long dnps = eng_analysis[ei].has_data ? eng_analysis[ei].nps : g_nps;
                char nps_s[32];
                if(dnps>=1000000) snprintf(nps_s,sizeof(nps_s),"%.1fM",dnps/1000000.0);
                else if(dnps>=1000) snprintf(nps_s,sizeof(nps_s),"%.0fK",dnps/1000.0);
                else snprintf(nps_s,sizeof(nps_s),"%lld", dnps);
                snprintf(st1,sizeof(st1),"D%d  %s  NPS:%s", disp_depth, evals, nps_s);
                int stats_y = sy + 4 + hdr_extra + UI_LINE_H + 2;
                dtxt(FRAME_W+11, stats_y+1, st1, 1, 0,0,0);
                dtxt(FRAME_W+10, stats_y, st1, 1, 200,200,200);
                /* PV */
                const char *pvsrc = eng_analysis[ei].has_data ? eng_analysis[ei].pv : g_pv_str;
                if(pvsrc[0]){
                    int pv_avail=panel_w-20;
                    if(pv_avail<60) pv_avail=60;
                    int max_ch=(int)(pv_avail/UI_ADV);
                    if(max_ch<10) max_ch=10; if(max_ch>180) max_ch=180;
                    char pv_display[200];
                    int pvlen=(int)strlen(pvsrc);
                    if(pvlen>max_ch) pvlen=max_ch;
                    strncpy(pv_display,pvsrc,pvlen); pv_display[pvlen]=0;
                    int pv1_y = stats_y + UI_LINE_H + 2;
                    dtxt(FRAME_W+11, pv1_y+1, pv_display, 1, 30,30,30);
                    dtxt(FRAME_W+10, pv1_y, pv_display, 1, 232,232,232);
                    if((int)strlen(pvsrc)>max_ch && per_h > (pv1_y - sy) + UI_LINE_H*3){
                        int pv2_start=max_ch;
                        int pv2_len=(int)strlen(pvsrc)-pv2_start;
                        if(pv2_len>max_ch) pv2_len=max_ch;
                        strncpy(pv_display,pvsrc+pv2_start,pv2_len); pv_display[pv2_len]=0;
                        int pv2_y = pv1_y + UI_LINE_H + 1;
                        dtxt(FRAME_W+11, pv2_y+1, pv_display, 1, 20,20,20);
                        dtxt(FRAME_W+10, pv2_y, pv_display, 1, 170,170,170);
                    }
                    char nbuf[64];
                    long long nn = eng_analysis[ei].has_data ? eng_analysis[ei].nodes : nodes_count;
                    if(nn>=1000000000) snprintf(nbuf,sizeof(nbuf),"Nodes %.1fG", nn/1e9);
                    else if(nn>=1000000) snprintf(nbuf,sizeof(nbuf),"Nodes %.1fM", nn/1e6);
                    else if(nn>=1000) snprintf(nbuf,sizeof(nbuf),"Nodes %.0fK", nn/1000.0);
                    else snprintf(nbuf,sizeof(nbuf),"Nodes %lld", nn);
                    int ny = sy + per_h - (UI_LINE_H+4);
                    dtxt(FRAME_W+10, ny, nbuf, 1, 120,120,120);
                    char evb2[16]; snprintf(evb2,sizeof(evb2),"%s", evals);
                    if(is_blue) dtxt(FRAME_W+panel_w-75, ny, evb2, 1, 80,130,220);
                    else dtxt(FRAME_W+panel_w-75, ny, evb2, 1, 255,165,0);
                } else {
                    dtxt(FRAME_W+10, sy+4+2*(UI_LINE_H+2), "(no analysis yet)", 1, 110,110,120);
                }
            } else {
                dtxt(FRAME_W+10, sy+4+(UI_LINE_H+2), "(waiting — not to move)", 1, 110,110,120);
                if(eng_analysis[ei].has_data){
                    char evals[24]; int evforside = evfl ? -eng_analysis[ei].eval : eng_analysis[ei].eval;
                    if(evforside>9000) strcpy(evals,"M+");
                    else if(evforside<-9000) strcpy(evals,"M-");
                    else snprintf(evals,sizeof(evals),"%+.2f", evforside/100.0);
                    dtxt(FRAME_W+panel_w-75, sy+per_h-(UI_LINE_H+4), evals, 1, 120,120,120);
                }
            }
            sy+=per_h+6;
            if(pi==0){
                draw_eval_panel_v14(sy, sw, dyn_panel_h);
                sy+=dyn_panel_h+6;
            }
        }
        if(npan==0){
            draw_eval_panel_v14(sy, sw, dyn_panel_h);
            sy+=dyn_panel_h+6;
        }
    }
    // Uniform MOVES panel — same width/height as the other sidebar panels
    int moves_w=sw-8;
    frect(FRAME_W+4,sy,moves_w,dyn_panel_h,0,0,0);
    orect(FRAME_W+4,sy,moves_w,dyn_panel_h,60,60,60);
    dtxt(FRAME_W+8,sy+4,"MOVES",1,100,180,220);
    {
        const char *opn=current_opening_name();
        if(opn[0]){
            char ob[64]; snprintf(ob,sizeof(ob),"%s",opn);
            int maxch=(int)((moves_w-90)/UI_ADV); if(maxch<10)maxch=10;
            if((int)strlen(ob)>maxch){ob[maxch-3]=0;strcat(ob,"...");}
            dtxt(FRAME_W+8+70,sy+4,ob,1,255,220,120);
        }
    }
    static const char*PCL[7]={"","","N","B","R","Q","K"};
    int right_edge=FRAME_W+4+moves_w-8;
    int line_h=UI_LINE_H+1;
    int panel_inner_y=sy+4+UI_LINE_H+8;
    int panel_inner_h=dyn_panel_h-(panel_inner_y-sy)-2;
    int lines_avail=panel_inner_h/line_h; if(lines_avail<1)lines_avail=1;
    /* v12.4: horizontal flow instead of a fixed two-column grid — wraps by actual
       text width so it always fits the panel, however narrow, instead of needing
       the window widened to show every move. Two passes: first find the smallest
       'start' whose wrapped rendering still fits in lines_avail, then draw it. */
    #define MV_TOKEN(i,tokbuf) do{ \
        Move *_m=&hist[i].m; int _p=bb_piece_at_rc(&hist[i].bb,_m->fr,_m->fc); int _tp=abs(_p); \
        char _mb[16]; \
        if(_m->castle==1||_m->castle==3)strcpy(_mb,"O-O"); \
        else if(_m->castle==2||_m->castle==4)strcpy(_mb,"O-O-O"); \
        else{sprintf(_mb,"%s%c%d",(_tp>1?PCL[_tp]:""),'a'+_m->tc,8-_m->tr);if(_m->promo){strcat(_mb,"=");strcat(_mb,PCL[_m->promo]);}} \
        if((i)%2==0)sprintf(tokbuf,"%d.%s",(i)/2+1,_mb); else strcpy(tokbuf,_mb); \
    }while(0)
    int start=0;
    for(start=0; start<hist_n; start++){
        int cx=FRAME_W+8, lines=1;
        for(int i=start;i<hist_n;i++){
            char tok[20]; MV_TOKEN(i,tok);
            int tw=(int)(strlen(tok)*UI_ADV+UI_ADV);
            if(cx+tw>right_edge){cx=FRAME_W+8;lines++;}
            cx+=tw;
        }
        if(lines<=lines_avail) break;
    }
    int cx=FRAME_W+8;
    int cur_y=panel_inner_y;
    for(int i=start;i<hist_n;i++){
        char tok[20]; MV_TOKEN(i,tok);
        int tw=(int)(strlen(tok)*UI_ADV+UI_ADV);
        if(cx+tw>right_edge){ cx=FRAME_W+8; cur_y+=line_h; if(cur_y+line_h > sy+dyn_panel_h) break; }
        int R2=170,G2=170,B2=170;if(i==hist_n-1){R2=255;G2=165;B2=0;}
        dtxt(cx,cur_y,tok,1,R2,G2,B2);
        cx+=tw;
    }
    sy+=dyn_panel_h+6;
    #undef MV_TOKEN

    // v14: FEN panel — dynamic, click to copy (between MOVES and PONDER)
    {
        update_cached_fen();
        int fx=FRAME_W+4, fy=sy, fw=sw-8, fh=FEN_H;
        fen_panel_x=fx; fen_panel_y=fy; fen_panel_w=fw; fen_panel_h=fh;
        frect(fx,fy,fw,fh,0,0,0);
        int is_hover = (mx>=fx && mx<fx+fw && my>=fy && my<fy+fh);
        orect(fx,fy,fw,fh, is_hover?255:70, is_hover?165:70, is_hover?0:70);
        dtxt(fx+6,fy+4,"FEN",1,255,165,0);
        dtxt(fx+6+ (int)(3*UI_ADV),fy+4,"(click to copy)",1,90,90,90);
        const char *copy_lbl="[Copy]";
        int copy_w=(int)(strlen(copy_lbl)*UI_ADV + 6);
        int copy_x=fx+fw-copy_w-6, copy_y=fy+4;
        int copy_hover = (mx>=copy_x && mx<copy_x+copy_w && my>=copy_y && my<copy_y+ UI_LINE_H);
        dtxt(copy_x, copy_y, copy_lbl,1, copy_hover?255:180, copy_hover?255:200, copy_hover?255:180);
        char fen_disp[256];
        int avail_w = fw - 12;
        int maxch = (int)(avail_w / UI_ADV);
        if(maxch<10) maxch=10;
        if(maxch> (int)sizeof(fen_disp)-1) maxch=sizeof(fen_disp)-1;
        strncpy(fen_disp, cached_fen, maxch);
        fen_disp[maxch]=0;
        if((int)strlen(cached_fen) > maxch){
            if(maxch>=3){ fen_disp[maxch-3]='.'; fen_disp[maxch-2]='.'; fen_disp[maxch-1]='.'; }
        }
        dtxt(fx+6, fy+4+UI_LINE_H+2, fen_disp,1, is_hover?255:200, is_hover?255:200, is_hover?255:200);
        sy+=fh+6;
    }
    /* v13: Enhanced ponder status indicator at bottom — clearly shows ON/OFF + RUNNING */
    {
        int ponder_any = ponder_running || uci_ponder_alive || uci_ponder_waiting;
        int by = MENU_H + sh - 32;
        int bx = FRAME_W+4, bw = sw-8, bh=UI_LINE_H+8;
        frect(bx, by, bw, bh, ponder_any?16:12, ponder_any?12:12, 12);
        if(ponder_any) orect(bx,by,bw,bh, 255,165,0);
        else if(!use_ponder) orect(bx,by,bw,bh, 80,80,80);
        else orect(bx,by,bw,bh, 70,70,70);
        char pbuf[80];
        if(!use_ponder){
            snprintf(pbuf,sizeof(pbuf),"PONDER OFF");
            dtxt(bx+8, by+7, pbuf, 1, 200,200,200);
            dtxt(bx+8 + (int)(strlen(pbuf)*UI_ADV) + 8, by+7, "(Settings)", 1, 110,110,110);
        } else if(ponder_any){
            int blink = (SDL_GetTicks()/350)%2;
            // fixed brackets, animate only star inside so surrounding text doesn't shift
            const char *pre="PONDER ON [";
            dtxt(bx+8, by+7, pre, 1, 255,180,40);
            int pre_w=(int)(strlen(pre)*UI_ADV);
            dtxt(bx+8+pre_w, by+7, blink?"*":" ", 1, 255,180,40);
            dtxt(bx+8+pre_w+(int)UI_ADV, by+7, "]", 1, 255,180,40);
            char rest[16]; snprintf(rest,sizeof(rest)," d%d", g_best_depth);
            int rest_x=bx+8+pre_w+(int)(2*UI_ADV);
            dtxt(rest_x, by+7, rest, 1, 255,180,40);
            int after_w=rest_x + (int)(strlen(rest)*UI_ADV);
            if(uci_ponder_alive) dtxt(after_w, by+7, " UCI", 1, 255,200,80);
            else if(ponder_running) dtxt(after_w, by+7, " CPU", 1, 200,200,200);
        } else {
            snprintf(pbuf,sizeof(pbuf),"PONDER ON [IDLE]");
            dtxt(bx+8, by+7, pbuf, 1, 180,160,120);
        }
    }
}

/* v13 CMD: bottom engine log — larger CMD text, below board — with tabs */
static void draw_bottom_log(void){
    int rw=real_w>0?real_w:WIN_W, rh=real_h>0?real_h:WIN_H;
    int y0 = rh - LOG_H;
    if(y0 < MENU_H+FRAME_H) y0 = MENU_H+FRAME_H;
    frect(0,y0,rw,LOG_H, 10,10,10);
    SDL_SetRenderDrawColor(ren,70,70,70,255);
    SDL_RenderDrawLine(ren,0,y0,rw,y0);
    // tabs — Out1 / Out2 = per-engine raw UCI, Log = a1 corner aligned
    int tab_w=70, tab_h=RAW_LINE_H+8;
    for(int t=0;t<3;t++){
        int tx=COORD_W + t*(tab_w+6), ty=y0;
        int active=(t==bottom_log_tab);
        frect(tx,ty,tab_w,tab_h, active?28:10, active?20:10, active?0:10);
        if(active) orect(tx,ty,tab_w,tab_h,255,165,0); else orect(tx,ty,tab_w,tab_h,60,60,60);
        const char *lab = t==0?"Out1": t==1?"Out2": "Log";
        dtxt_raw(tx+12, ty+4, lab,1, active?255:150, active?165:150, active?0:150);
    }
    dtxt_raw(rw-170, y0+4, "per-engine UCI output",1, 90,90,90);
    if(bottom_log_tab==2){
        // LOG tab — game/status messages only (engine traffic lives in Out1/Out2;
        // moves are shown in the MOVES panel, not here).
        int ly=y0+tab_h+4;
        int box_h=LOG_H-(tab_h+4)-4;
        frect(COORD_W,ly,rw - COORD_W - 8,box_h,0,0,0);
        orect(COORD_W,ly,rw - COORD_W - 8,box_h,55,55,55);
        int line_h=12;
        int max_lines=(box_h-8)/line_h; if(max_lines<1) max_lines=1;
        int cnt=0; for(int i=0;i<bottom_log_n;i++) if(bottom_log_engine(bottom_log_lines[i])==-1) cnt++;
        int show_n = cnt<max_lines ? cnt : max_lines;
        int box_w = rw - COORD_W - 8;
        int maxch = (int)((box_w - 12)/RAW_ADV); if(maxch<20) maxch=20; if(maxch>120) maxch=120;
        int drawn=0;
        for(int i=bottom_log_n-1; i>=0 && drawn<show_n; i--){
            char *ln = bottom_log_lines[i];
            if(!ln[0] || bottom_log_engine(ln)!=-1) continue;
            char disp[140]; strncpy(disp, ln, sizeof(disp)-1); disp[sizeof(disp)-1]=0;
            if((int)strlen(disp) > maxch){ disp[maxch-3]=0; strcat(disp,"..."); }
            dtxt_raw(COORD_W+4, ly+4+(show_n-1-drawn)*line_h, disp,1, 180,180,180);
            drawn++;
        }
        if(cnt==0) dtxt_raw(COORD_W+4, ly+4, "(log empty — engine and game messages will appear here)",1, 110,110,110);
        return;
    }
    /* v13: Out1 / Out2 tabs — raw UCI protocol for Engine 1 / Engine 2, like
       Arena's per-engine Output windows. Each tab shows only its own engine's
       RECV/SEND/ENGINE lines. */
    {
        int out_ei = (bottom_log_tab==0)?0:1;
        int ly=y0+tab_h+4;
        int box_h=LOG_H-(tab_h+4)-4;
        frect(COORD_W,ly,rw - COORD_W - 8,box_h,0,0,0);
        orect(COORD_W,ly,rw - COORD_W - 8,box_h,55,55,55);
        int line_h=12;
        int max_lines=(box_h-8)/line_h; if(max_lines<1) max_lines=1;
        int cnt=0; for(int i=0;i<bottom_log_n;i++) if(bottom_log_engine(bottom_log_lines[i])==out_ei) cnt++;
        int show_n = cnt<max_lines ? cnt : max_lines;
        int box_w2 = rw - COORD_W - 8;
        int maxch2 = (int)((box_w2 - 12)/RAW_ADV); if(maxch2<20) maxch2=20; if(maxch2>120) maxch2=120;
        int drawn=0;
        for(int i=bottom_log_n-1; i>=0 && drawn<show_n; i--){
            char *ln = bottom_log_lines[i];
            if(!ln[0] || bottom_log_engine(ln)!=out_ei) continue;
            int colR=180,colG=180,colB=180;
            if(strstr(ln,"[SEND")){ colR=150;colG=200;colB=255; }
            else if(strstr(ln,"[RECV")){ colR=255;colG=210;colB=150; }
            else if(strstr(ln,"[ENGINE")){ colR=255;colG=160;colB=120; }
            char disp2[140]; strncpy(disp2, ln, sizeof(disp2)-1); disp2[sizeof(disp2)-1]=0;
            if((int)strlen(disp2) > maxch2){ disp2[maxch2-3]=0; strcat(disp2,"..."); }
            dtxt_raw(COORD_W+4, ly+4+(show_n-1-drawn)*line_h, disp2,1, colR,colG,colB);
            drawn++;
        }
        if(cnt==0) dtxt_raw(COORD_W+4, ly+4, bottom_log_tab==0? "(no Engine 1 (E1) output yet)":"(no Engine 2 (E2) output yet)",1, 110,110,110);
    }
}

/* ===================== RENDER ===================== */
static void render(int mx,int my){
    { int rw=real_w>0?real_w:WIN_W, rh=real_h>0?real_h:WIN_H; frect(0,0,rw,rh,0,0,0); }
    /* CMD: faint dot grid on black */
    SDL_SetRenderDrawColor(ren,14,14,14,255);
    for(int gx=0; gx< (real_w>0?real_w:WIN_W); gx+=24) for(int gy=MENU_H; gy<(real_h>0?real_h:WIN_H); gy+=24) SDL_RenderDrawPoint(ren,gx,gy);
    const Theme*th=&THEMES[cur_theme];
    int fl=flip_board^(player_color==BLACK?1:0);

    /* ---- CMD flat frame — includes clock bar above board ---- */
    int fx=COORD_W, fy=MENU_H, fw=FRAME_W - COORD_W, fh=FRAME_H;
    frect(fx,fy,fw,fh, 18,18,18);
    orect(fx,fy,fw,fh, 72,72,72);
    /* clock bar background */
    frect(fx, fy, fw, CLOCK_BAR_H, 0,0,0);
    SDL_SetRenderDrawColor(ren,70,70,70,255);
    SDL_RenderDrawLine(ren, fx, fy+CLOCK_BAR_H, fx+fw, fy+CLOCK_BAR_H);
    /* draw clocks centered in bar — moved inward, no turn text */
    {
        int cw = 22*4+26; /* clock width from draw_clock */
        int cy = fy + (CLOCK_BAR_H-30)/2 + 4;
        int wx = BOARD_OX + 2*SQ_SIZE - cw/2;
        int bx = BOARD_OX + 6*SQ_SIZE - cw/2;
        if(wx < fx+8) wx = fx+8;
        if(bx + cw > fx+fw-8) bx = fx+fw-8 - cw;
        draw_clock(wx+10, cy, clk_w, turn==WHITE&&!game_over);
        draw_clock(bx, cy, clk_b, turn==BLACK&&!game_over);
    }
    if(cur_theme<=4) orect(BOARD_OX-2,BOARD_OY-2,BRD+4,BRD+4, 255,165,0); /* orange accent for CMD */
    else if(cur_theme==7) orect(BOARD_OX-2,BOARD_OY-2,BRD+4,BRD+4, 90,140,220); /* blue accent for CMD Blue */
    else orect(BOARD_OX-2,BOARD_OY-2,BRD+4,BRD+4, 90,90,90);
    orect(BOARD_OX-1,BOARD_OY-1,BRD+2,BRD+2, 44,44,44);

    /* ---- Squares ---- */
    for(int r=0;r<8;r++) for(int c=0;c<8;c++){
        int px,py; sq2px(r,c,&px,&py);
        if((r+c)%2==0)
            frect(px,py,SQ_SIZE,SQ_SIZE,th->lr,th->lg,th->lb);
        else
            frect(px,py,SQ_SIZE,SQ_SIZE,th->dr,th->dg,th->db);
    }

    /* ---- CMD coordinates — minimal gray, no glow ---- */
    const char*files="abcdefgh"; char buf[4];
    for(int i=0;i<8;i++){
        int fi=fl?7-i:i, ri=fl?i:7-i;
        buf[0]=files[fi]; buf[1]=0;
        int lx=BOARD_OX+i*SQ_SIZE+SQ_SIZE/2-4;
        int ly=BOARD_OY+BRD+6;
        dtxt(lx-2,ly-3,buf,1, 150,150,150);
        buf[0]='1'+ri; buf[1]=0;
        int nx=COORD_W/2-8;
        int ny=BOARD_OY+i*SQ_SIZE+SQ_SIZE/2-6;
        dtxt(nx-4,ny-3,buf,1, 150,150,150);
    }
    /* ---- CMD Highlights — orange/gray/red minimal ---- */
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    if(lm_fr>=0){
        int c2[2][2]={{lm_fr,lm_fc},{lm_tr,lm_tc}};
        for(int i=0;i<2;i++){int px,py;sq2px(c2[i][0],c2[i][1],&px,&py);
            SDL_SetRenderDrawColor(ren,255,165,0,95);SDL_Rect sq={px,py,SQ_SIZE,SQ_SIZE};SDL_RenderFillRect(ren,&sq);}
    }
    if(!game_over && bb_inchk(&B, turn==WHITE?WC:BC)){
        BB king=B.pieces[turn==WHITE?WC:BC][BB_K];
        if(king){
            int r=BB_LSB(king)/8, c=BB_LSB(king)%8;
            int px,py;sq2px(r,c,&px,&py);SDL_SetRenderDrawColor(ren,220,20,20,130);SDL_Rect sq={px,py,SQ_SIZE,SQ_SIZE};SDL_RenderFillRect(ren,&sq);
        }
    }
    if(sel_r>=0&&!promo_pending){
        int px,py;sq2px(sel_r,sel_c,&px,&py);SDL_SetRenderDrawColor(ren,255,165,0,80);SDL_Rect sr={px,py,SQ_SIZE,SQ_SIZE};SDL_RenderFillRect(ren,&sr);
        Move leg[64]; int lc;
        Move all_moves[256]; int n=bb_gen_moves(&B, turn==WHITE?WC:BC, all_moves);
        lc=0;
        for(int i=0;i<n;i++) if(all_moves[i].fr==sel_r && all_moves[i].fc==sel_c){
            BBoard bc; memcpy(&bc,&B,sizeof bc);
            bb_do(&bc,&all_moves[i],turn==WHITE?WC:BC);
            if(!bb_inchk(&bc,turn==WHITE?WC:BC)) leg[lc++]=all_moves[i];
        }
        for(int i=0;i<lc;i++){int lpx,lpy;sq2px(leg[i].tr,leg[i].tc,&lpx,&lpy);
            if(B.occ[2]&(1ULL<<(leg[i].tr*8+leg[i].tc))||leg[i].ep_cap>=0){SDL_SetRenderDrawColor(ren,180,0,0,110);SDL_Rect mr={lpx,lpy,SQ_SIZE,SQ_SIZE};SDL_RenderFillRect(ren,&mr);}
            else{SDL_SetRenderDrawColor(ren,150,150,150,55);int cx=lpx+SQ_SIZE/2,cy=lpy+SQ_SIZE/2;int cr=SQ_SIZE/7;int cr2=cr*cr;for(int dy=-cr;dy<=cr;dy++){int dx=(int)sqrt((double)(cr2-dy*dy));SDL_RenderDrawLine(ren,cx-dx,cy+dy,cx+dx,cy+dy);}}
        }
    }
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    for(int col=0;col<2;col++) for(int t=0;t<6;t++){
        BB bb=B.pieces[col][t];
        while(bb){int sq=BB_LSB(bb);BB_POP(bb);int r=sq/8,c=sq%8;
            if(anim_active && r==anim_to_r && c==anim_to_c) continue; /* v12: hidden while sliding in */
            if(drag_active && r==drag_r && c==drag_c) continue;       /* v12: hidden while dragged */
            int p=(col==WC?1:-1)*(t+1);draw_piece_at(p,r,c);}
    }
    /* v12: slide-in animation for the last move */
    if(anim_active){
        Uint32 elapsed=SDL_GetTicks()-anim_start;
        float t=elapsed/(float)ANIM_MS;
        if(t>=1.0f) anim_active=0;
        else{
            int px1,py1,px2,py2;
            sq2px(anim_from_r,anim_from_c,&px1,&py1);
            sq2px(anim_to_r,anim_to_c,&px2,&py2);
            float ease=1.0f-(1.0f-t)*(1.0f-t);
            int cx=(int)(px1+(px2-px1)*ease);
            int cy=(int)(py1+(py2-py1)*ease);
            draw_piece_px(anim_piece,cx,cy);
        }
    }
    /* v12: dragged piece follows the cursor */
    if(drag_active){
        draw_piece_px(drag_piece,drag_mx-SQ_SIZE/2,drag_my-SQ_SIZE/2);
    }
    /* Draw analysis arrows */
    for(int ai=0;ai<arrow_count;ai++){
        int px1,py1,px2,py2;
        sq2px(arrows[ai][0],arrows[ai][1],&px1,&py1);
        sq2px(arrows[ai][2],arrows[ai][3],&px2,&py2);
        draw_arrow(px1+SQ_SIZE/2,py1+SQ_SIZE/2,px2+SQ_SIZE/2,py2+SQ_SIZE/2,0,180,80,180);
    }
    /* Draw arrow preview during drag */
    if(arrow_dragging){
        int px1,py1;
        sq2px(arrow_fr,arrow_fc,&px1,&py1);
        draw_arrow(px1+SQ_SIZE/2,py1+SQ_SIZE/2,arrow_mx,arrow_my,0,160,70,140);
    }
    draw_sidebar(mx,my);
    draw_bottom_log();
    if(retro_scanlines || retro_vignette || retro_crt_curve) draw_retro_overlays();
    draw_menus(mx,my);
    if(!ai_thinking)SDL_SetWindowTitle(win,msg);
    /* v9: FEN dialog overlay */
    if(custom_games_dialog_active) draw_custom_games_dialog();
    if(fen_dialog_active) draw_fen_dialog();
    if(uci_opts_dialog_active) draw_uci_options_dialog();
    /* v14.1 FIX: stropt_dialog (string/spin value editor) must draw AFTER the
       options dialog — drawing it before hid it completely behind the options
       overlay (it sits fully inside its rect), so string/spin rows looked
       dead; worse, while the invisible stropt stayed open every click was
       swallowed by the modal guard, freezing even check toggles. Same bug
       class as the tourney-manager/path_dialog order fixed above. */
    if(stropt_dialog_active) draw_stropt_dialog();
    /* v14 fix: the tournament manager panel must draw before path_dialog,
       since "+ Add engine..." inside the manager opens path_dialog on top
       of it -- drawing tourney manager last was hiding that dialog every
       frame, making it look like clicks did nothing. */
    if(tourney_mgr_active) draw_tourney_manager();
    if(path_dialog_active) draw_path_dialog();
    SDL_RenderPresent(ren);
    if(promo_pending)render_promo();
}

/* ===================== GAME LOGIC ===================== */
static int has_legal(int col){
    Move mv[256]; int n=bb_gen_moves(&B,(col==WHITE?WC:BC),mv);
    for(int i=0;i<n;i++){
        BBoard bc; memcpy(&bc,&B,sizeof bc);
        bb_do(&bc,&mv[i],(col==WHITE?WC:BC));
        if(!bb_inchk(&bc,(col==WHITE?WC:BC))) return 1;
    }
    return 0;
}

static void check_end(void){
    if(B.fifty>=100){game_over=1;strcpy(msg,"Draw - 50 move rule!");uci_dbg_log("GAME",-1,msg);tourney_record_result();return;}
    int wp=0,bp=0,wn=0,bn=0,wb=0,bbb=0,wmaj=0,bmaj=0;
    for(int col=0;col<2;col++) for(int t=0;t<6;t++){
        BB pieces=B.pieces[col][t];
        int cnt=__builtin_popcountll(pieces);
        if(t==BB_P){ if(col==WC)wp=cnt; else bp=cnt; }
        else if(t==BB_N){ if(col==WC)wn=cnt; else bn=cnt; }
        else if(t==BB_B){ if(col==WC)wb=cnt; else bbb=cnt; }
        else if(t==BB_R||t==BB_Q){ if(col==WC)wmaj+=cnt; else bmaj+=cnt; }
        /* BB_K is deliberately excluded: a king is always present and must
           never count as "material" for the insufficient-material check. */
    }
    if(!wp&&!bp&&!wmaj&&!bmaj&&wn+wb<=1&&bn+bbb<=1){game_over=1;strcpy(msg,"Draw - insufficient material!");uci_dbg_log("GAME",-1,msg);tourney_record_result();return;}
    if(hist_n>=4){
        uint64_t cur_hash=B.hash;
        int cnt=1;
        int start=hist_n%2;
        for(int i=start;i<hist_n;i+=2) if(hist[i].hash==cur_hash) cnt++;
        if(cnt>=3){game_over=1;strcpy(msg,"Draw - threefold repetition!");uci_dbg_log("GAME",-1,msg);tourney_record_result();return;}
    }
    if(!has_legal(turn)){
        game_over=1;
        if(bb_inchk(&B, turn==WHITE?WC:BC)){
            if(turn==WHITE)
                sprintf(msg,"Black wins by checkmate!");
            else
                sprintf(msg,"White wins by checkmate!");
        } else {
            strcpy(msg,"Stalemate - draw!");
        }
        uci_dbg_log("GAME",-1,msg);
        tourney_record_result();
    }
}

static void apply(Move*m){
    if(m->cap||m->ep_cap>=0)beep(350,80);else beep(600,50);
    if(m->castle)beep(480,55);
    do_move_full(m);
    log_move_played(hist_n-1); /* v13: record the move in the bottom LOG tab */
    clock_started=1; /* v12.9: first move of the game starts the clocks */
    if(hist_n-1>=0 && hist_n-1<EVAL_HIST_MAX) eval_hist[hist_n-1]=g_best_eval; /* v12: for sidebar sparkline */
    if(turn==WHITE)clk_w+=increment+last_move_overhead_ms;
    else clk_b+=increment+last_move_overhead_ms;
    last_move_overhead_ms=0; /* consumed -- don't double-credit a later human/built-in move */
    turn=-turn;
    /* sync game_hist_hashes */
    if(game_hist_n<MAX_GAME_HIST) game_hist_hashes[game_hist_n++]=B.hash;
    if(bb_inchk(&B, turn==WHITE?WC:BC))beep(880,100);
    draw_offered=0;
    check_end();
    if(game_over) cancel_uci_ponder(); /* v12.7: stop any UCI ponder when the game ends */
    else uci_ponder_opponent_moved(m); /* v12.7: opponent moved — UCI ponder hit or miss */
    if(!game_over)sprintf(msg,both_human?"Your move":(turn==player_color?"Your move":"Computer thinking..."));
}

/* v12: shared move-attempt logic, used by classic click-click AND drag-drop */
static int ponder_ready; /* forward decl: full definition later near pondering code */
static int attempt_move(int fr,int fc,int tr,int tc){
    Move leg[64];int lc=0;
    Move all_moves[256];int n=bb_gen_moves(&B,turn==WHITE?WC:BC,all_moves);
    for(int i=0;i<n;i++) if(all_moves[i].fr==fr&&all_moves[i].fc==fc&&all_moves[i].tr==tr&&all_moves[i].tc==tc){
        BBoard bc;memcpy(&bc,&B,sizeof bc);bb_do(&bc,&all_moves[i],turn==WHITE?WC:BC);
        if(!bb_inchk(&bc,turn==WHITE?WC:BC)) leg[lc++]=all_moves[i];
    }
    for(int i=0;i<lc;i++){
        if(ponder_running)stop_pondering();
        if(ai_thinking){stop_ai();ponder_ready=0;}
        if(leg[i].promo){promo_fr=fr;promo_fc=fc;promo_tr=tr;promo_tc=tc;promo_pending=1;sel_r=sel_c=-1;return 1;}
        apply(&leg[i]);return 1;
    }
    return 0;
}

static void apply_ai(void){
    Move ai=ai_result;if(ai.fr<0){check_end();return;}
    Move leg[64]; int lc;
    Move all_moves[256]; int n=bb_gen_moves(&B, turn==WHITE?WC:BC, all_moves);
    lc=0;
    for(int i=0;i<n;i++) if(all_moves[i].fr==ai.fr&&all_moves[i].fc==ai.fc&&all_moves[i].tr==ai.tr&&all_moves[i].tc==ai.tc){
        BBoard bc; memcpy(&bc,&B,sizeof bc);
        bb_do(&bc,&all_moves[i],turn==WHITE?WC:BC);
        if(!bb_inchk(&bc,turn==WHITE?WC:BC)) leg[lc++]=all_moves[i];
    }
    if(lc==0){
        uci_dbg_log("WARN", 99, "apply_ai: engine move not legal on GUI board -> restarting search");
        start_ai_move();return;
    }
    int mover=uci_last_mover_ei; /* capture BEFORE apply(): the ponder check may set it for the next reply */
    apply(&leg[0]);
    if(!game_over){
        /* v12.7: after a UCI engine's move, let it think ahead on the
           opponent's time (go ponder) — this is what makes engines with
           their own permanent brain (Sila etc.) reply instantly, like in
           Arena. The built-in ponder is skipped for UCI movers (the UCI
           ponder replaces it; otherwise both could fire a double reply). */
        /* FIX v13: gate UCI (external engine) ponder on the GUI's use_ponder
       switch too — previously only the built-in start_pondering() honored
       the menu Ponder ON/OFF, so turning ponder OFF did nothing for added
       external engines. */
    if(mover>=0 && use_ponder && uci_engine_ponder_enabled(mover)) start_uci_ponder(mover);
        if(!aivsai && mover<0) start_pondering();
    }
}

static void do_move_full(Move*m){
    if(hist_n<MAX_HIST){hist[hist_n].m=*m;memcpy(&hist[hist_n].bb,&B,sizeof B);hist[hist_n].turn=turn;hist[hist_n].hash=B.hash;hist_n++;}
    /* v12 fix: bb_piece_at() returns an UNSIGNED piece type, so m->cap is never
       negative — the old "if(m->cap>0)" branch always took the same path and
       dumped every capture into cap_w[]. Use 'turn' (the side moving right now,
       i.e. the side doing the capturing) to file it correctly instead.
       cap_w[] = pieces White has captured, cap_b[] = pieces Black has captured. */
    if(m->cap){int tp=abs(m->cap);if(turn==WHITE)cap_w[tp]++;else cap_b[tp]++;}
    if(m->ep_cap>=0){if(turn==WHITE)cap_w[PAWN]++;else cap_b[PAWN]++;}
    int moved_piece=bb_piece_at_rc(&B,m->fr,m->fc); /* v12: grab piece before mutating board, for animation */
    bb_do(&B,m,(turn==WHITE?WC:BC));
    lm_fr=m->fr;lm_fc=m->fc;lm_tr=m->tr;lm_tc=m->tc;
    /* v12: kick off the slide-in animation for this move */
    anim_piece=moved_piece;anim_from_r=m->fr;anim_from_c=m->fc;anim_to_r=m->tr;anim_to_c=m->tc;
    anim_start=SDL_GetTicks();anim_active=1;
}
/* v13: push a human-readable "N. move" line to the bottom LOG tab for every
   move played, regardless of whether an external UCI engine is running —
   previously the LOG tab only ever received raw UCI SEND/RECV traffic, so it
   stayed empty in built-in-engine games and looked broken. */
static void log_move_played(int idx){
    if(idx<0||idx>=hist_n) return;
    Move *m=&hist[idx].m;
    int p=bb_piece_at_rc(&hist[idx].bb,m->fr,m->fc);
    int tp=abs(p);
    static const char*L_PCL[7]={"","","N","B","R","Q","K"};
    char mb[32];
    if(m->castle==1||m->castle==3) strcpy(mb,"O-O");
    else if(m->castle==2||m->castle==4) strcpy(mb,"O-O-O");
    else{
        int is_cap=m->cap||m->ep_cap>=0;
        snprintf(mb,sizeof(mb),"%s%c%d%c%c%d",(tp>1?L_PCL[tp]:""),'a'+m->fc,8-m->fr,is_cap?'x':'-','a'+m->tc,8-m->tr);
        if(m->promo){strcat(mb,"=");strcat(mb,L_PCL[m->promo]);}
    }
    char line[64];
    if(idx%2==0) snprintf(line,sizeof(line),"%d. %s",idx/2+1,mb);
    else snprintf(line,sizeof(line),"%d... %s",idx/2+1,mb);
    uci_dbg_log("MOVE",-1,line);
}
/* v12.1: recompute captured-piece counts from scratch off the remaining history.
   Needed after undo, since do_undo() rewinds the board but previously left
   cap_w[]/cap_b[] untouched — any later capture would then pile on top of the
   undone one instead of starting clean (e.g. "+3" then an even trade showing
   "+6" instead of settling back to "0"). */
static void recompute_captures(void){
    memset(cap_w,0,sizeof cap_w); memset(cap_b,0,sizeof cap_b);
    for(int i=0;i<hist_n;i++){
        Move *m=&hist[i].m; int mv_turn=hist[i].turn;
        if(m->cap){int tp=abs(m->cap); if(mv_turn==WHITE)cap_w[tp]++; else cap_b[tp]++;}
        if(m->ep_cap>=0){if(mv_turn==WHITE)cap_w[PAWN]++; else cap_b[PAWN]++;}
    }
}
static void do_undo(void){
    if(hist_n<=0)return;
    uci_dbg_log("GAME",-1,"Move undone");
    cancel_uci_ponder(); /* v12.7: an undo invalidates any UCI ponder */
    hist_n--;
    anim_active=0;drag_active=0; /* v12: cancel any in-flight animation/drag */
    memcpy(&B,&hist[hist_n].bb,sizeof B);
    turn=hist[hist_n].turn;
    recompute_captures(); /* v12.1: keep the captured-pieces tray in sync with undo */
    /* restore game_hist_hashes */
    game_hist_n=0;
    for(int i=0;i<hist_n;i++) if(game_hist_n<MAX_GAME_HIST) game_hist_hashes[game_hist_n++]=hist[i].hash;
    if(game_hist_n<MAX_GAME_HIST) game_hist_hashes[game_hist_n++]=B.hash;
    if(hist_n>0){lm_fr=hist[hist_n-1].m.fr;lm_fc=hist[hist_n-1].m.fc;lm_tr=hist[hist_n-1].m.tr;lm_tc=hist[hist_n-1].m.tc;}
    else lm_fr=lm_fc=lm_tr=lm_tc=-1;
    sel_r=sel_c=-1;game_over=0;draw_offered=0;
    sprintf(msg,both_human?"Your move":(turn==player_color?"Your move":"Computer thinking..."));
}


/* ===================== AI THREAD (v8.2 adapted) ===================== */

static void build_pv(BBoard *root, int col_idx) {
    g_pv_str[0]='\0';
    if(g_best.fr<0) return;
    BBoard b=*root; int ci=col_idx;
    BBoard bc; memcpy(&bc,&b,sizeof bc);
    bb_do(&bc,&g_best,ci);
    if(bb_inchk(&bc,ci)) return;
    char ms[8]; sprintf(ms,"%c%d%c%d",'a'+g_best.fc,8-g_best.fr,'a'+g_best.tc,8-g_best.tr);
    strcpy(g_pv_str,ms);
    b=bc; ci^=1; uint64_t hash=bc.hash;
    for(int ply=1;ply<12;ply++){
        TTEntry *e=&transposition_table[hash&(TT_SIZE-1)];
        if(e->key!=hash||e->move.fr<0) break;
        Move mv[256]; int n=bb_gen_moves(&b,ci,mv); int found=0;
        for(int i=0;i<n;i++){
            if(mv[i].fr==e->move.fr&&mv[i].fc==e->move.fc&&mv[i].tr==e->move.tr&&mv[i].tc==e->move.tc){
                memcpy(&bc,&b,sizeof bc); bb_do(&bc,&mv[i],ci);
                if(!bb_inchk(&bc,ci)){
                    sprintf(ms," %c%d%c%d",'a'+mv[i].fc,8-mv[i].fr,'a'+mv[i].tc,8-mv[i].tr);
                    strcat(g_pv_str,ms); b=bc; hash=bc.hash; ci^=1; found=1;
                }
                break;
            }
        }
        if(!found) break;
    }
}

static void show_think(int d, int ev, Move m) {
    if(m.fr<0) return;
    g_best_depth = d;
    Uint32 elapsed = SDL_GetTicks() - start_time;
    if(elapsed > 0) g_nps = (long long)(nodes_count * 1000ULL / elapsed);
    char evs[16];
    if(ev>9000)sprintf(evs,"M+%d",(MATE-ev+1)/2);
    else if(ev<-9000)sprintf(evs,"M-%d",(MATE+ev+1)/2);
    else sprintf(evs,"%+.2f",ev/100.0);
    int rem=(int)((player_color>0?clk_b:clk_w)/1000);
    char buf[512];
    if(ai_is_ponder) sprintf(buf,"Pondering d%d %s | %s",d,evs,g_pv_str);
    else sprintf(buf,"d%d %s | %s | %d:%02d",d,evs,g_pv_str,rem/60,rem%60);
    SDL_SetWindowTitle(win,buf);
    /* v13: mirror to per-engine panel for Built-in */
    eng_analysis_update(ENG_BUILTIN_IDX, "StrongEngine (Built-in)", d, ev, g_pv_str, g_nps, nodes_count, 1);
}

static void init_time_management_gui(int my_time, int my_inc) {
    start_time=SDL_GetTicks();
    stop_search=0; nodes_count=0;
    if(++tt_age==0)tt_age=1;
    for(int s=0;s<2;s++)for(int f=0;f<64;f++)for(int t=0;t<64;t++)history[s][f][t]>>=1;
    memset(killer,0,sizeof killer); memset(killer_cnt,0,sizeof killer_cnt);
    memset(counter,0,sizeof counter); memset(cmh,0,sizeof(cmh));
    memset(cap_history,0,sizeof(cap_history)); /* v8.3 */
    target_time=my_time/40+(int)(my_inc*0.8);
    hard_limit=my_time/5;
    if(my_time<5000){target_time=my_time/10;hard_limit=my_time/3;}
    if(target_time<20)target_time=20; if(hard_limit<50)hard_limit=50;
}

static int ai_thread_func(void *data) {
    (void)data;
    BBoard W=B;
    int ai_color=aivsai?turn:-player_color;
    int col_idx=(ai_color==WHITE)?WC:BC;

    g_best.fr=-1; g_best_eval=0;

    /* sync game history for repetition detection */
    game_hist_n=0;
    for(int i=0;i<hist_n&&game_hist_n<MAX_GAME_HIST;i++)
        game_hist_hashes[game_hist_n++]=hist[i].hash;

    if(!ai_is_ponder){
        /* v12.9 FIX: use the clock of the side TO MOVE. The old
           (player_color==WHITE)?clk_b:clk_w read the human's clock, which
           is correct only in human-vs-AI games; in AI-vs-AI (player_color
           forced to WHITE) the built-in WHITE engine was budgeting with
           BLACK's clock, and the built-in BLACK engine with BLACK's clock
           too. turn-based is correct in every mode. */
        int my_time=(turn==WHITE)?(int)clk_w:(int)clk_b;
        init_time_management_gui(my_time, increment);
    } else {
        start_time=SDL_GetTicks(); stop_search=0; nodes_count=0;
        target_time=0; hard_limit=180000;
    }

    int prev_eval=0;
    Move prev_best; prev_best.fr=-1;
    int last_score=0, window=40;
    int max_search_depth=MAX_DEPTH;

    for(int d=1;d<=max_search_depth&&!ai_cancel&&!stop_search;d++){
        int best_score=-INF; Move best_move; best_move.fr=-1;
        int alpha=(d>=4&&g_best.fr>=0)?last_score-window:-INF;
        int beta=(d>=4&&g_best.fr>=0)?last_score+window:INF;

        while(1){
            best_score=-INF; best_move.fr=-1;
            int ia=alpha, ib=beta;
            Move all_moves[256]; int mc=bb_gen_moves(&W,col_idx,all_moves);
            score_moves(all_moves,mc,col_idx,0,NULL,&W);
            /* Put TT move first */
            TTEntry *tte=&transposition_table[W.hash&(TT_SIZE-1)];
            Move ttm; ttm.fr=-1;
            if(tte->key==W.hash&&tte->move.fr>=0) ttm=tte->move;
            if(ttm.fr>=0){for(int i=0;i<mc;i++)if(all_moves[i].fr==ttm.fr&&all_moves[i].fc==ttm.fc&&all_moves[i].tr==ttm.tr&&all_moves[i].tc==ttm.tc){all_moves[i].score=1000000;Move t=all_moves[0];all_moves[0]=all_moves[i];all_moves[i]=t;break;}}

            for(int i=0;i<mc&&!ai_cancel&&!stop_search;i++){
                BBoard Wt; memcpy(&Wt,&W,sizeof Wt);
                bb_do(&Wt,&all_moves[i],col_idx);
                if(bb_inchk(&Wt,col_idx)) continue;
                uint64_t nh=Wt.hash;
                int score=-bb_negamax(&Wt,d-1,-ib,-ia,col_idx^1,nh,0,NULL);
                if(score>best_score){best_score=score;best_move=all_moves[i];}
                if(score>ia)ia=score;
                if(ia>=ib) break;
            }
            if(ai_cancel||stop_search) break;
            if(best_score<=alpha){alpha-=window;window*=2;if(window>1000){alpha=-INF;beta=INF;}}
            else if(best_score>=beta){beta+=window;window*=2;if(window>1000){alpha=-INF;beta=INF;}}
            else{last_score=best_score;window=40;break;}
            if(alpha<=-INF&&beta>=INF) break;
        }
        if(ai_cancel||stop_search) break;
        if(best_move.fr>=0){g_best=best_move;g_best_eval=(col_idx==WC)?best_score:-best_score;} /* v12.4: normalize eval to White's perspective, fixes eval graph/bar flipping sign by whose turn it is */
        build_pv(&W,col_idx);
        show_think(d,g_best_eval,g_best);
        if(!ai_is_ponder){
            Uint32 elapsed=SDL_GetTicks()-start_time;
            if(elapsed>=hard_limit) break;
            int unstable=0;
            if(best_move.fr>=0&&prev_best.fr>=0){
                if(best_move.fr!=prev_best.fr||best_move.fc!=prev_best.fc)unstable=1;
                if(abs(best_score-prev_eval)>25)unstable=1;
            } else if(prev_best.fr<0&&best_move.fr>=0) unstable=1;
            if(elapsed>=target_time&&!unstable) break;
            if(elapsed>(Uint32)(target_time*0.6)) break;
        } else {
            Uint32 elapsed=SDL_GetTicks()-start_time;
            if(elapsed>=hard_limit){stop_search=1;break;}
        }
        if(best_move.fr>=0){prev_eval=best_score;prev_best=best_move;}
        if(abs(g_best_eval)>40000) break;
    }
    ai_result=g_best; ai_done=1; ai_thinking=0;
    eng_analysis[ENG_BUILTIN_IDX].is_thinking=0;
    return 0;
}

static void stop_ai(void) {
    if(ai_thinking){
        ai_cancel=1; stop_search=1;
        int wait=0;
        while(ai_thinking&&wait<600){SDL_Delay(5);wait++;}
    }
    ai_cancel=0; stop_search=0; ai_done=0; ai_thinking=0;
    for(int _ei=0;_ei<3;_ei++) eng_analysis[_ei].is_thinking=0;
    cancel_uci_ponder(); /* v12.7: stopping the AI also aborts any UCI ponder */
}

static void start_ai_move(void) {
    if(game_over) return;
    if(analysis_mode) return;
    if(uci_ponder_waiting) return; /* v12.7: a ponderhit reply is on its way — do not start a new search */
    if(ai_thinking) stop_ai();
    ai_done=0; ai_thinking=1; ai_cancel=0; ai_is_ponder=0;
    uci_last_mover_ei = -1; /* v12.7: track which engine produces the next ai_result */

    /* v10: pick engine based on whose turn it is.
       v12.7 FIX: honor the Tournament-menu W/B player assignment not only
       in a real tournament but also in a plain "Game -> AI vs AI" match
       (aivsai). Previously a plain AI-vs-AI game always used the single
       selected UCI engine (use_uci_engine/active_engine) for BOTH sides,
       so after loading Engine 1 the built-in engine was silently ignored
       even when the user had set W: Built-in / B: Engine 1. */
#ifdef USE_BOOK
    if(use_book){
        Move bm=book_move();
        if(bm.fr>=0){ai_result=bm;ai_done=1;ai_thinking=0;SDL_SetWindowTitle(win,"Book!");return;}
    }
#endif

    int want_uci = use_uci_engine;
    int which_ei = active_engine; /* default: selected engine */
    
    if(tourney_active || aivsai){
        int tp = (turn==WHITE) ? tourney_player[0] : tourney_player[1];
        /* 0=built-in, 1=UCI engine 1, 2=UCI engine 2, 3=human */
        if(tp == 1){ want_uci = 1; which_ei = 0; }
        else if(tp == 2){ want_uci = 1; which_ei = 1; }
        else { want_uci = 0; }
    }

    if(want_uci){
        if(uci_eng[which_ei].ready && UCI_VALID(which_ei)){
            start_uci_ai_move(which_ei);
            return;
        }
        /* GUI FIX: previously this fell through to the BUILT-IN engine
           SILENTLY whenever the configured UCI engine (e.g. Sila) wasn't
           ready/valid at that instant -- no log entry, no on-screen sign,
           just a different search running that move. Since the built-in
           engine shares its code with "Strong", the game LOOKED normal and
           uci_debug.log simply had nothing for that engine, making it seem
           like "only Strong played". Log it loudly instead so a fallback
           is never silent again. */
        char _fbmsg[128];
        snprintf(_fbmsg,sizeof _fbmsg,
            "start_ai_move: engine slot %d (ready=%d valid=%d) not usable -- "
            "falling back to BUILT-IN engine for this move!",
            which_ei, uci_eng[which_ei].ready, UCI_VALID(which_ei));
        uci_dbg_log("WARN", which_ei, _fbmsg);
    }

    eng_analysis[ENG_BUILTIN_IDX].is_thinking=1;
    if(!eng_analysis[ENG_BUILTIN_IDX].has_data){
        eng_analysis_update(ENG_BUILTIN_IDX, "StrongEngine (Built-in)", 0, 0, "", 0, 0, 1);
    }
    SDL_CreateThread(ai_thread_func,"AI",NULL);
}

/* v8.3.1: Infinite analysis mode - separate thread with infinite time */
static int analysis_thread_func(void *data) {
    (void)data;
    while(analysis_mode && !ai_cancel) {
        BBoard W=B;
        int col_idx=(turn==WHITE)?WC:BC;
        g_best.fr=-1; g_best_eval=0;
        game_hist_n=0;
        for(int i=0;i<hist_n&&game_hist_n<MAX_GAME_HIST;i++)
            game_hist_hashes[game_hist_n++]=hist[i].hash;
        start_time=SDL_GetTicks(); stop_search=0; nodes_count=0;
        target_time=0; hard_limit=0x7FFFFFFF;
        int last_score=0, window=40;
        Move prev_best; prev_best.fr=-1;
        for(int d=1;d<=MAX_DEPTH&&!ai_cancel&&!stop_search&&analysis_mode;d++){
            int best_score=-INF; Move best_move; best_move.fr=-1;
            int alpha=(d>=4&&g_best.fr>=0)?last_score-window:-INF;
            int beta=(d>=4&&g_best.fr>=0)?last_score+window:INF;
            while(1){
                best_score=-INF; best_move.fr=-1;
                int ia=alpha, ib=beta;
                Move all_moves[256]; int mc=bb_gen_moves(&W,col_idx,all_moves);
                score_moves(all_moves,mc,col_idx,0,NULL,&W);
                TTEntry *tte=&transposition_table[W.hash&(TT_SIZE-1)];
                Move ttm; ttm.fr=-1;
                if(tte->key==W.hash&&tte->move.fr>=0) ttm=tte->move;
                if(ttm.fr>=0){for(int i=0;i<mc;i++)if(all_moves[i].fr==ttm.fr&&all_moves[i].fc==ttm.fc&&all_moves[i].tr==ttm.tr&&all_moves[i].tc==ttm.tc){all_moves[i].score=1000000;Move t=all_moves[0];all_moves[0]=all_moves[i];all_moves[i]=t;break;}}
                for(int i=0;i<mc&&!ai_cancel&&!stop_search;i++){
                    BBoard Wt; memcpy(&Wt,&W,sizeof Wt);
                    bb_do(&Wt,&all_moves[i],col_idx);
                    if(bb_inchk(&Wt,col_idx)) continue;
                    int score=-bb_negamax(&Wt,d-1,-ib,-ia,col_idx^1,Wt.hash,0,NULL);
                    if(score>best_score){best_score=score;best_move=all_moves[i];}
                    if(score>ia)ia=score;
                    if(ia>=ib) break;
                }
                if(ai_cancel||stop_search) break;
                if(best_score<=alpha){alpha-=window;window*=2;if(window>1000){alpha=-INF;beta=INF;}}
                else if(best_score>=beta){beta+=window;window*=2;if(window>1000){alpha=-INF;beta=INF;}}
                else{last_score=best_score;window=40;break;}
                if(alpha<=-INF&&beta>=INF) break;
            }
            if(ai_cancel||stop_search) break;
            if(best_move.fr>=0){g_best=best_move;g_best_eval=(col_idx==WC)?best_score:-best_score;prev_best=best_move;} /* v12.4: normalize to White's perspective, same fix as main search */
            build_pv(&W,col_idx);
            show_think(d,g_best_eval,g_best);
            g_best_depth=d;
            if(abs(g_best_eval)>40000) break;
        }
    }
    ai_thinking=0; ai_done=0;
    return 0;
}

static void start_analysis(void) {
    if(ai_thinking) stop_ai();
    stop_pondering();
    analysis_mode=1; ai_cancel=0; stop_search=0;
    ai_thinking=1; ai_done=0; ai_is_ponder=0;
    SDL_CreateThread(analysis_thread_func,"Analysis",NULL);
    strcpy(msg,"Analyzing... (I to stop)");
    SDL_SetWindowTitle(win,"Chess GUI v14 - Analyzing");
}

static void stop_analysis(void) {
    analysis_mode=0; ai_cancel=1; stop_search=1;
    int wait=0;
    while(ai_thinking&&wait<400){SDL_Delay(5);wait++;}
    ai_cancel=0; stop_search=0; ai_thinking=0; ai_done=0;
    SDL_SetWindowTitle(win,"Chess GUI v14");
}


static BBoard ponder_after;
static Move ponder_opp_move;
static Move ponder_resp_move;
static int ponder_depth=0;
static int ponder_ready=0;
static SDL_Thread *ponder_thread=NULL;
/* ponder_running declared above */

static int moves_equal(Move *a, Move *b) {
    return a->fr==b->fr&&a->fc==b->fc&&a->tr==b->tr&&a->tc==b->tc&&a->promo==b->promo;
}
static int ponder_thread_func(void *data) {
    (void)data;
    BBoard W=ponder_after;
    int ai_color=-player_color;
    int col_idx=(ai_color==WHITE)?WC:BC;
    ponder_resp_move.fr=-1; ponder_ready=0;
    int last_score=0, window=40;
    start_time=SDL_GetTicks(); nodes_count=0;
    hard_limit=180000; target_time=0; stop_search=0;

    /* sync game history */
    game_hist_n=0;
    for(int i=0;i<hist_n&&game_hist_n<MAX_GAME_HIST;i++)
        game_hist_hashes[game_hist_n++]=hist[i].hash;

    for(int d=1;d<=MAX_DEPTH&&!ai_cancel&&!stop_search;d++){
        int best_score=-INF; Move best_move; best_move.fr=-1;
        int alpha=(d>=4&&ponder_resp_move.fr>=0)?last_score-window:-INF;
        int beta=(d>=4&&ponder_resp_move.fr>=0)?last_score+window:INF;
        while(1){
            best_score=-INF; best_move.fr=-1; int ia=alpha,ib=beta;
            Move mv[256]; int mc=bb_gen_moves(&W,col_idx,mv);
            for(int i=0;i<mc;i++)for(int j=i+1;j<mc;j++){
                if((mv[j].cap?MAT[mv[j].cap]:0)>(mv[i].cap?MAT[mv[i].cap]:0)){Move t=mv[i];mv[i]=mv[j];mv[j]=t;}}
            for(int i=0;i<mc&&!ai_cancel&&!stop_search;i++){
                BBoard Wt;memcpy(&Wt,&W,sizeof Wt);bb_do(&Wt,&mv[i],col_idx);
                if(bb_inchk(&Wt,col_idx))continue;
                uint64_t nh=Wt.hash;
                int score=-bb_negamax(&Wt,d-1,-ib,-ia,col_idx^1,nh,0,NULL);
                if(score>best_score){best_score=score;best_move=mv[i];}
                if(score>ia)ia=score;
                if(ia>=ib)break;
            }
            if(ai_cancel||stop_search)goto ponderexit;
            if(best_score<=alpha){alpha-=window;window*=2;if(window>1000){alpha=-INF;beta=INF;}}
            else if(best_score>=beta){beta+=window;window*=2;if(window>1000){alpha=-INF;beta=INF;}}
            else{last_score=best_score;window=40;break;}
            if(alpha<=-INF&&beta>=INF)break;
        }
        if(ai_cancel||stop_search)break;
        if(best_move.fr>=0){ponder_resp_move=best_move;ponder_depth=d;ponder_ready=1;g_best_depth=d;}
    }
ponderexit:
    ponder_running=0;
    return 0;
}
static void stop_pondering(void) {
    if(ponder_running){
        ai_cancel=1; stop_search=1;
        SDL_WaitThread(ponder_thread,NULL);
        ponder_thread=NULL; ai_cancel=0; stop_search=0;
        ponder_running=0; ponder_ready=0;
    }
}
static void start_pondering(void) {
    if(aivsai||game_over||ponder_running||!use_ponder) return;
    Move predicted; predicted.fr=-1;
    int opp=(player_color==WHITE)?WC:BC;
    if(g_pv_str[0]){
        char tmp_pv[200];strncpy(tmp_pv,g_pv_str,199);
        char *tok=strtok(tmp_pv," ");
        if(tok)tok=strtok(NULL," ");
        if(tok&&strlen(tok)>=4){
            int fc=tok[0]-'a',fr=8-(tok[1]-'0');
            int tc=tok[2]-'a',tr=8-(tok[3]-'0');
            int promo=0;
            if(tok[4]=='n')promo=KNIGHT;else if(tok[4]=='b')promo=BISHOP;
            else if(tok[4]=='r')promo=ROOK;else if(tok[4]=='q')promo=QUEEN;
            Move mv[256];int mc=bb_gen_moves(&B,opp,mv);
            for(int i=0;i<mc;i++){
                if(mv[i].fr==fr&&mv[i].fc==fc&&mv[i].tr==tr&&mv[i].tc==tc&&mv[i].promo==promo){
                    BBoard bc;memcpy(&bc,&B,sizeof bc);bb_do(&bc,&mv[i],opp);
                    if(!bb_inchk(&bc,opp)){predicted=mv[i];break;}
                }
            }
        }
    }
    if(predicted.fr<0){
        TTEntry *e=&transposition_table[B.hash&(TT_SIZE-1)];
        if(e->key==B.hash&&e->move.fr>=0){
            Move mv[256];int mc=bb_gen_moves(&B,opp,mv);
            for(int i=0;i<mc;i++){
                if(mv[i].fr==e->move.fr&&mv[i].fc==e->move.fc&&mv[i].tr==e->move.tr&&mv[i].tc==e->move.tc){
                    BBoard bc;memcpy(&bc,&B,sizeof bc);bb_do(&bc,&mv[i],opp);
                    if(!bb_inchk(&bc,opp)){predicted=mv[i];break;}
                }
            }
        }
    }
    if(predicted.fr<0) return;
    ponder_opp_move=predicted;
    memcpy(&ponder_after,&B,sizeof B);
    bb_do(&ponder_after,&predicted,opp);
    if(bb_inchk(&ponder_after,opp)) return;
    ponder_running=1;
    ponder_thread=SDL_CreateThread(ponder_thread_func,"Ponder",NULL);
}

/* ===================== SAVE PGN ===================== */
static void save_pgn(void){
    OPENFILENAMEA ofn={0};
    char fname[260]="chess_game.pgn";
    ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=NULL;
    ofn.lpstrFile=fname;
    ofn.nMaxFile=260;
    ofn.lpstrFilter="PGN Files\0*.pgn\0All Files\0*.*\0";
    ofn.nFilterIndex=1;
    ofn.lpstrTitle="Save PGN";
    ofn.Flags=OFN_OVERWRITEPROMPT;
    if(!GetSaveFileNameA(&ofn)) return;
    /* auto-append .pgn if missing */
    {int flen=(int)strlen(fname);
    if(flen<4 || _stricmp(fname+flen-4,".pgn")!=0){strcat(fname,".pgn");}}

    FILE *f=fopen(fname,"w");
    if(!f){ sprintf(msg,"Failed to save: %s",fname); return; }
    time_t t=time(NULL);struct tm *tm2=localtime(&t);
    fprintf(f,"[Event \"Casual Game\"]\n");
    fprintf(f,"[Site \"Chess\"]\n");
    fprintf(f,"[Date \"%04d.%02d.%02d\"]\n",tm2->tm_year+1900,tm2->tm_mon+1,tm2->tm_mday);
    fprintf(f,"[Round \"1\"]\n");
    fprintf(f,"[White \"%s\"]\n",player_color==WHITE?"Player":"Computer");
    fprintf(f,"[Black \"%s\"]\n",player_color==BLACK?"Player":"Computer");
    fprintf(f,"[Result \"*\"]\n\n");
    BBoard cur_board; memcpy(&cur_board,&hist[0].bb,sizeof cur_board);
    int cur_turn=WHITE; int move_num=1;
    for(int i=0;i<hist_n;i++){
        if(cur_turn==WHITE) fprintf(f,"%d.",move_num);
        Move *m=&hist[i].m;
        char san[64]="";
        int piece_type=abs(bb_piece_at_rc(&cur_board,m->fr,m->fc));
        int capture=(m->cap!=0||m->ep_cap>=0);
        if(m->castle==1||m->castle==3) strcpy(san,"O-O");
        else if(m->castle==2||m->castle==4) strcpy(san,"O-O-O");
        else{
            if(piece_type==PAWN){
                if(capture){char pf[3]={(char)('a'+m->fc),'x',0};strcat(san,pf);}
            } else {
                const char *pl="";
                switch(piece_type){case KNIGHT:pl="N";break;case BISHOP:pl="B";break;case ROOK:pl="R";break;case QUEEN:pl="Q";break;case KING:pl="K";break;default:break;}
                strcat(san,pl);
                int ci2=(cur_turn==WHITE)?WC:BC;
                Move amb[256];int na=bb_gen_moves(&cur_board,ci2,amb);
                int same_file=0,same_rank=0,need_disambig=0;
                for(int ai=0;ai<na;ai++){
                    if(amb[ai].fr==m->fr&&amb[ai].fc==m->fc)continue;
                    if(amb[ai].tr!=m->tr||amb[ai].tc!=m->tc)continue;
                    if(abs(bb_piece_at_rc(&cur_board,amb[ai].fr,amb[ai].fc))!=piece_type)continue;
                    BBoard bt;memcpy(&bt,&cur_board,sizeof bt);bb_do(&bt,&amb[ai],ci2);
                    if(bb_inchk(&bt,ci2))continue;
                    need_disambig=1;
                    if(amb[ai].fc==m->fc) same_file=1;
                    if(amb[ai].fr==m->fr) same_rank=1;
                }
                if(need_disambig){
                    char dis[3]={0};
                    if(!same_file){dis[0]=(char)('a'+m->fc);}
                    else if(!same_rank){dis[0]=(char)('0'+(8-m->fr));}
                    else{dis[0]=(char)('a'+m->fc);dis[1]=(char)('0'+(8-m->fr));}
                    strcat(san,dis);
                }
                if(capture)strcat(san,"x");
            }
            char dest[4]; sprintf(dest,"%c%d",'a'+m->tc,8-m->tr); strcat(san,dest);
            if(m->promo){
                const char *pp="";switch(m->promo){case KNIGHT:pp="N";break;case BISHOP:pp="B";break;case ROOK:pp="R";break;case QUEEN:pp="Q";break;}
                strcat(san,"=");strcat(san,pp);
            }
        }
        BBoard after;memcpy(&after,&cur_board,sizeof after);
        bb_do(&after,m,cur_turn==WHITE?WC:BC);
        int gives_check=bb_inchk(&after,cur_turn==WHITE?BC:WC);
        int gives_mate=0;
        if(gives_check){
            int opp_ci=(cur_turn==WHITE)?BC:WC;
            Move tmp2[256];int nt=bb_gen_moves(&after,opp_ci,tmp2);
            int has_any=0;
            for(int li=0;li<nt&&!has_any;li++){BBoard bt;memcpy(&bt,&after,sizeof bt);bb_do(&bt,&tmp2[li],opp_ci);if(!bb_inchk(&bt,opp_ci))has_any=1;}
            gives_mate=!has_any;
        }
        if(gives_mate)strcat(san,"#");
        else if(gives_check)strcat(san,"+");
        fprintf(f," %s",san);
        if(cur_turn==BLACK){fprintf(f,"\n");move_num++;}
        memcpy(&cur_board,&after,sizeof cur_board);
        cur_turn=-cur_turn;
    }
    if(hist_n%2==1) fprintf(f,"\n");
    fprintf(f,"*\n");
    fclose(f);
    sprintf(msg,"Saved: %s",fname);
}

/* ===================== LOAD PGN ===================== */
static void load_pgn(void){
    OPENFILENAMEA ofn={0};
    char fname[260]="";
    ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=NULL;
    ofn.lpstrFile=fname;
    ofn.nMaxFile=260;
    ofn.lpstrFilter="PGN Files\0*.pgn\0All Files\0*.*\0";
    ofn.nFilterIndex=1;
    ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
    if(!GetOpenFileNameA(&ofn)) return;

    FILE *f=fopen(fname,"r");
    if(!f){ strcpy(msg,"Failed to open PGN"); return; }

    fseek(f,0,SEEK_END);
    long sz=ftell(f);
    fseek(f,0,SEEK_SET);
    char *buf=malloc(sz+1);
    if(!buf){ fclose(f); strcpy(msg,"Out of memory"); return; }
    fread(buf,1,sz,f);
    buf[sz]=0;
    fclose(f);

    /* skip PGN tags and find the move section (after last ] line) */
    char *p=buf;
    char *last_tag=strrchr(buf,']');
    if(last_tag) p=last_tag+1;
    else p=buf; /* no tags — start from beginning */

    stop_ai(); stop_analysis(); stop_pondering();
    init_board();
    turn=WHITE;

    /* parse move tokens */
    int move_count=0;
    while(*p){
        /* skip move numbers like "1." "2." "12..." */
        if(*p>='1' && *p<='9'){
            char *dot=p;
            while(*dot && *dot>='0' && *dot<='9') dot++;
            if(*dot=='.'){ p=dot+1; continue; }
        }
        /* skip whitespace */
        if(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t'){ p++; continue; }

        /* end markers */
        if(*p=='1' && *(p+1)=='-' && *(p+2)=='0'){ break; }
        if(*p=='0' && *(p+1)=='-' && *(p+2)=='1'){ break; }
        if(*p=='1' && *(p+1)=='/' && *(p+2)=='2'){ break; }
        if(*p=='*'){ break; }

        /* extract one token (SAN move) */
        char token[32]="";
        int ti=0;
        while(*p && *p!=' ' && *p!='\n' && *p!='\r' && *p!='\t' && ti<30){
            token[ti++]=*p++;
        }
        token[ti]=0;
        if(ti==0) continue;

        /* find matching legal move by generating SAN for each and comparing */
        int col_idx=(turn==WHITE)?WC:BC;
        Move all_moves[256];
        int n=bb_gen_moves(&B,col_idx,all_moves);
        int found=0;
        for(int i=0;i<n;i++){
            /* check legality on a copy */
            BBoard bc;memcpy(&bc,&B,sizeof bc);
            bb_do(&bc,&all_moves[i],col_idx);
            if(bb_inchk(&bc,col_idx)) continue;

            /* generate SAN for this move */
            char san[32]="";
            int pt=abs(bb_piece_at_rc(&B,all_moves[i].fr,all_moves[i].fc));
            int is_cap=(all_moves[i].cap!=0) || (all_moves[i].ep_cap>=0);
            if(all_moves[i].castle==1||all_moves[i].castle==3) strcpy(san,"O-O");
            else if(all_moves[i].castle==2||all_moves[i].castle==4) strcpy(san,"O-O-O");
            else{
                if(pt==PAWN){
                    if(is_cap){
                        int slen=0;
                        san[slen++]=(char)('a'+all_moves[i].fc);
                        san[slen++]='x';
                        san[slen++]=(char)('a'+all_moves[i].tc);
                        san[slen++]=(char)('0'+(8-all_moves[i].tr));
                        san[slen]=0;
                    } else {
                        int slen=0;
                        san[slen++]=(char)('a'+all_moves[i].tc);
                        san[slen++]=(char)('0'+(8-all_moves[i].tr));
                        san[slen]=0;
                    }
                    if(all_moves[i].promo){
                        int slen=(int)strlen(san);
                        san[slen]='=';
                        san[slen+1]=" PNBRQK"[all_moves[i].promo];
                        san[slen+2]=0;
                    }
                } else {
                    const char *pn=" PNBRQK";
                    int slen=0;
                    san[slen++]=pn[pt];
                    san[slen]=0;
                    /* disambiguation: check if other pieces of same type can reach same square */
                    int need_file=0,need_rank=0;
                    for(int j=0;j<n;j++){
                        if(j==i) continue;
                        if(all_moves[j].tr!=all_moves[i].tr || all_moves[j].tc!=all_moves[i].tc) continue;
                        if(abs(bb_piece_at_rc(&B,all_moves[j].fr,all_moves[j].fc))!=pt) continue;
                        BBoard bc2;memcpy(&bc2,&B,sizeof bc2);
                        bb_do(&bc2,&all_moves[j],col_idx);
                        if(bb_inchk(&bc2,col_idx)) continue;
                        if(all_moves[j].fc!=all_moves[i].fc) need_file=1;
                        else if(all_moves[j].fr!=all_moves[i].fr) need_rank=1;
                        else { need_file=1; need_rank=1; }
                    }
                    if(need_file){san[slen++]=(char)('a'+all_moves[i].fc);}
                    if(need_rank){san[slen++]=(char)('0'+(8-all_moves[i].fr));}
                    if(is_cap){san[slen++]='x';}
                    san[slen++]=(char)('a'+all_moves[i].tc);
                    san[slen++]=(char)('0'+(8-all_moves[i].tr));
                    san[slen]=0;
                }
            }
            /* compare SAN with token (ignoring +/-/# at end) */
            char tok_clean[32]="";
            int tc=0;
            for(int k=0;token[k];k++) if(token[k]!='+' && token[k]!='#') tok_clean[tc++]=token[k];
            tok_clean[tc]=0;
            if(strcmp(san,tok_clean)==0){
                do_move_full(&all_moves[i]);
                turn=-turn;
                move_count++;
                found=1;
                break;
            }
        }
        if(!found){
            free(buf);
            sprintf(msg,"Parse error: %s",token);
            return;
        }
    }
    free(buf);
    sprintf(msg,"Loaded %d moves from %s",move_count,fname);SDL_SetWindowTitle(win,msg);

    /* Setup replay */
    pgn_replay_mode = 1;
    pgn_replay_index = hist_n;
    if(pgn_replay_moves) free(pgn_replay_moves);
    pgn_replay_moves = malloc(hist_n * sizeof(Move));
    pgn_replay_move_count = hist_n;
    for(int i=0;i<hist_n;i++) pgn_replay_moves[i]=hist[i].m;
}



/* ===================== PGN REPLAY ===================== */
static void pgn_replay_prev(void){
    if(!pgn_replay_mode || pgn_replay_index<=0){sprintf(msg,"REPLAY: prev skipped mode=%d idx=%d",pgn_replay_mode,pgn_replay_index);SDL_SetWindowTitle(win,msg);return;}
    do_undo();
    pgn_replay_index--;
    sprintf(msg,"REPLAY: prev ok idx=%d/%d",pgn_replay_index,pgn_replay_move_count);SDL_SetWindowTitle(win,msg);
}

static void pgn_replay_next(void){
    if(!pgn_replay_mode || pgn_replay_index>=pgn_replay_move_count) return;
    do_move_full(&pgn_replay_moves[pgn_replay_index]);
    turn=-turn;
    pgn_replay_index++;
    sprintf(msg,"REPLAY: next ok idx=%d/%d",pgn_replay_index,pgn_replay_move_count);SDL_SetWindowTitle(win,msg);
}

static void pgn_replay_first(void){
    if(!pgn_replay_mode) return;
    while(pgn_replay_index>0) pgn_replay_prev();
}

static void pgn_replay_last(void){
    if(!pgn_replay_mode) return;
    while(pgn_replay_index<pgn_replay_move_count) pgn_replay_next();
}

static void pgn_replay_auto_play(void){
    if(!pgn_replay_mode){sprintf(msg,"No PGN loaded");return;}
    if(pgn_replay_auto){
        pgn_replay_auto=0;
        sprintf(msg,"Replay stopped");SDL_SetWindowTitle(win,msg);
    } else {
        pgn_replay_first();
        pgn_replay_auto=1;
        pgn_replay_auto_last=SDL_GetTicks();
        sprintf(msg,"Replaying...");SDL_SetWindowTitle(win,msg);
    }
}

/* ===================== v10: UCI ENGINE SUBSYSTEM ===================== */

/* UCI protocol debug log — v12.6: writes every line sent to / received from
   each external UCI engine to uci_debug.log next to the exe, so issues with
   engines like Stockfish can be traced (position/go/bestmove). */
static FILE *uci_dbg = NULL;
static void uci_dbg_open(void){
    if(uci_dbg) return;
#ifdef _WIN32
    char p[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    if(n > 0 && n < MAX_PATH){
        char *sl = strrchr(p, '\\');
        if(sl){ sl[1] = 0; strncat(p, "uci_debug.log", MAX_PATH-1);
            /* v12.11: auto-rotate log when >5 MB to prevent unbounded growth */
            FILE *ft = fopen(p, "rb");
            if(ft){ fseek(ft, 0, SEEK_END); long sz = ftell(ft); fclose(ft);
                if(sz > 5*1024*1024){ /* truncate */ uci_dbg = fopen(p, "w"); if(uci_dbg){fprintf(uci_dbg,"[Log rotated — previous was %ld MB]\n", sz/(1024*1024)); fflush(uci_dbg);} }
            }
            if(!uci_dbg) uci_dbg = fopen(p, "a");
        }
    }
    if(!uci_dbg) uci_dbg = fopen("uci_debug.log", "a");
#else
    uci_dbg = fopen("uci_debug.log", "a");
#endif
    if(uci_dbg){
        fprintf(uci_dbg, "\n===== UCI debug %s =====\n", __DATE__);
        fflush(uci_dbg);
    }
}
static void uci_dbg_log(const char *dir, int ei, const char *line){
    if(!uci_dbg) uci_dbg_open();
    if(uci_dbg){ fprintf(uci_dbg, "[%s e%d] %s\n", dir, ei, line); fflush(uci_dbg); }
    // mirror to bottom log tab, EXCEPT plain moves (those live in the MOVES
    // panel, not the log) and skipped huge position lines
    if(strcmp(dir,"MOVE")==0) return;
    if(line && strlen(line)<120){
        char tmp[140]; snprintf(tmp,sizeof(tmp),"[%s %d] %s", dir, ei, line);
        bottom_log_push(tmp);
    } else if(line && strncmp(line,"position",8)==0){
        char tmp[140]; snprintf(tmp,sizeof(tmp),"[%s %d] position ... (%d bytes)", dir, ei, (int)strlen(line));
        bottom_log_push(tmp);
    }
}

/* ---- low-level send / recv (platform-specific, per engine slot) ---- */
#ifdef _WIN32

static int uci_send_raw(int ei, const char *s){
    if(ei<0||ei>=MAX_ENGINES||!uci_hin[ei]) return 0;
    uci_dbg_open();
    uci_dbg_log("SEND", ei, s);
    /* v12.6 FIX: was 1100 bytes — but the position command is up to 8192
       bytes (UCIArgs.position_cmd). Long games were silently truncated, so
       the engine's board diverged from the GUI's and its bestmove never
       matched a legal move -> the move was never applied and the search
       restarted forever. Use the full command buffer (newline-terminated). */
    size_t sl = strlen(s);
    if(sl > 8192) sl = 8192;
    DWORD written;
    if(!WriteFile(uci_hin[ei], s, (DWORD)sl, &written, NULL)) return 0;
    char nl = '\n';
    if(!WriteFile(uci_hin[ei], &nl, 1, &written, NULL)) return 0;
    return 1;
}
/* v12.7 FIX: keep partial lines across calls. The old reader returned a
   partial line when its 20ms read window expired mid-line (engine writes
   while the deadline is about to hit), and the NEXT call treated the tail
   of that line as a fresh line. A "bestmove" line split this way lost its
   "b" prefix, was never matched, and the GUI waited until its timeout and
   restarted the search forever (the intermittent "engine doesn't answer /
   game freezes" bug — confirmed in uci_debug.log as "NO valid bestmove").
   Partial data is now stashed per engine and prepended to the next call. */
static char uci_partial[MAX_ENGINES][4096];
static int  uci_partial_len[MAX_ENGINES];

static int uci_recv_line(int ei, char *buf, int maxlen, int timeout_ms){
    if(ei<0||ei>=MAX_ENGINES||!uci_hout[ei]) return 0;
    DWORD deadline=GetTickCount()+(DWORD)timeout_ms, avail, r;
    int pos=0;
    /* prepend any partial line left over from the previous call */
    if(uci_partial_len[ei]>0){
        int cp=uci_partial_len[ei];
        if(cp>maxlen-1) cp=maxlen-1;
        memcpy(buf,uci_partial[ei],cp);
        pos=cp; uci_partial_len[ei]=0;
    }
    while((int)(GetTickCount()-deadline)<0 && pos<maxlen-1){
        if(!PeekNamedPipe(uci_hout[ei],NULL,0,NULL,&avail,NULL)) return 0;
        if(avail>0){
            char c; if(!ReadFile(uci_hout[ei],&c,1,&r,NULL)||r==0) return 0;
            if(c=='\n'){buf[pos]=0;uci_dbg_log("RECV",ei,buf);return 1;}
            if(c!='\r'){
                buf[pos++]=c;
                if(pos>=maxlen-1) break; /* tail handled on the next call */
            }
        } else Sleep(5);
    }
    if(pos>0){
        /* no newline yet: stash the partial line for the next call */
        if(pos>=(int)sizeof(uci_partial[ei])) pos=(int)sizeof(uci_partial[ei])-1;
        memcpy(uci_partial[ei],buf,pos);
        uci_partial_len[ei]=pos;
        if(pos<80){char pb[96];memcpy(pb,buf,pos);pb[pos]=0;uci_dbg_log("PART",ei,pb);}
        buf[0]=0;
    }
    return 0;
}

/* v13: crash watchdog — logs when an engine process dies (Arena-style),
   so the debug log is useful for diagnosing engine crashes. Engine stderr
   is already merged into the same pipe as stdout, so the actual crash text
   shows up as RECV lines above this message. */
static int uci_crash_logged[MAX_ENGINES]={0};
static void uci_watchdog(void){
    for(int ei=0;ei<MAX_ENGINES;ei++){
        if(uci_eng[ei].ready && uci_hproc[ei] && !uci_crash_logged[ei]){
            DWORD c;
            if(GetExitCodeProcess(uci_hproc[ei],&c) && c!=STILL_ACTIVE){
                char m[300];
                snprintf(m,sizeof m,
                    "ENGINE CRASHED/EXITED (exit code %u) — last stderr lines above; engine path: %s",
                    (unsigned)c, uci_eng[ei].path);
                uci_dbg_log("CRASH", ei, m);
                uci_crash_logged[ei]=1;
            }
        }
    }
}

static int uci_spawn_engine(int ei, const char *path){
    if(ei<0||ei>=MAX_ENGINES) return 0;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE chin_r,chin_w,cout_r,cout_w;
    if(!CreatePipe(&chin_r,&chin_w,&sa,0)) return 0;
    if(!CreatePipe(&cout_r,&cout_w,&sa,0)){
        CloseHandle(chin_r);CloseHandle(chin_w);return 0;
    }
    SetHandleInformation(chin_w,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(cout_r,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOA si={0}; si.cb=sizeof(si);
    si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE;
    si.hStdInput=chin_r; si.hStdOutput=cout_w; si.hStdError=cout_w;
    PROCESS_INFORMATION pi={0};
    char cmd[260]; strncpy(cmd,path,259);
    if(!CreateProcessA(NULL,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)){
        CloseHandle(chin_r);CloseHandle(chin_w);
        CloseHandle(cout_r);CloseHandle(cout_w);
        { char m[300]; snprintf(m,sizeof m,"CreateProcess FAILED for %s (Win32 error %u)", path, (unsigned)GetLastError()); uci_dbg_log("ENGINE", ei, m); }
        return 0;
    }
    CloseHandle(chin_r); CloseHandle(cout_w); CloseHandle(pi.hThread);
    uci_hproc[ei]=pi.hProcess; uci_hin[ei]=chin_w; uci_hout[ei]=cout_r;
    uci_crash_logged[ei]=0;
    uci_partial_len[ei]=0; uci_partial[ei][0]=0;
    { char m[300]; snprintf(m,sizeof m,"engine started: %s", path); uci_dbg_log("ENGINE", ei, m); }

    /* Parse UCI options during handshake */
    UCIOption *opts = uci_eng[ei].options;
    int *nopts = &uci_eng[ei].num_options;
    *nopts = 0;
    strncpy(uci_eng[ei].path, path, 255);
    uci_eng[ei].name[0]=0;

    uci_send_raw(ei, "uci");
    char buf[512]; int waited=0;
    while(waited<3000){
        if(uci_recv_line(ei, buf, 512, 50)){
            /* extract engine name from "id name ..." */
            if(strncmp(buf,"id name ",8)==0 && !uci_eng[ei].name[0]){
                strncpy(uci_eng[ei].name, buf+8, 127);
                /* v13: also mirror to per-engine panel */
                strncpy(eng_analysis[ei].name, buf+8, 127);
            }
            if(strncmp(buf,"option name ",12)==0 && *nopts < MAX_UCI_OPTIONS){
                UCIOption *o = &opts[*nopts];
                memset(o, 0, sizeof(*o));
                char *p = buf + 12;
                char *tp = strstr(p, " type ");
                if(tp){
                    int nlen = tp - p;
                    if(nlen > 127) nlen = 127;
                    memcpy(o->name, p, nlen);
                    o->name[nlen] = 0;
                    tp += 6;
                    if(strncmp(tp,"check",5)==0){
                        o->type = UOPT_CHECK;
                        char *dv = strstr(tp,"default ");
                        if(dv) o->def_check = o->cur_check = (strncmp(dv+8,"true",4)==0);
                    } else if(strncmp(tp,"spin",4)==0){
                        o->type = UOPT_SPIN;
                        char *dv = strstr(tp,"default "); if(dv) o->def_spin = o->cur_spin = atoi(dv+8);
                        char *mn = strstr(tp,"min "); if(mn) o->spin_min = atoi(mn+4);
                        char *mx = strstr(tp,"max "); if(mx) o->spin_max = atoi(mx+4);
                    } else if(strncmp(tp,"combo",5)==0){
                        o->type = UOPT_COMBO;
                        char *dv = strstr(tp,"default ");
                        char *vp = strstr(tp," var ");
                        o->combo_count = 0;
                        if(dv && (!vp || dv < vp)){
                            char *end = vp ? vp : (tp+strlen(tp));
                            int vl = end - (dv+8);
                            if(vl > 127) vl = 127;
                            memcpy(o->combo_vars[0], dv+8, vl);
                            o->combo_vars[0][vl] = 0;
                            o->combo_def_idx = o->combo_cur_idx = 0;
                            o->combo_count = 1;
                        }
                        while(vp && o->combo_count < MAX_COMBO_VARS){
                            vp += 5;
                            char *next = strstr(vp, " var ");
                            int vl = next ? (next - vp) : (int)strlen(vp);
                            if(vl > 127) vl = 127;
                            memcpy(o->combo_vars[o->combo_count], vp, vl);
                            o->combo_vars[o->combo_count][vl] = 0;
                            if(dv){
                                char defval[128];
                                char *dend = strstr(dv+8, " var ");
                                int dlen = dend ? (dend-(dv+8)) : (int)strlen(dv+8);
                                if(dlen > 127) dlen = 127;
                                memcpy(defval, dv+8, dlen);
                                defval[dlen] = 0;
                                if(strcmp(o->combo_vars[o->combo_count], defval)==0)
                                    o->combo_def_idx = o->combo_cur_idx = o->combo_count;
                            }
                            o->combo_count++;
                            vp = next;
                        }
                    } else if(strncmp(tp,"string",6)==0){
                        o->type = UOPT_STRING;
                        char *dv = strstr(tp,"default ");
                        if(dv){ strncpy(o->def_str, dv+8, 255); strncpy(o->cur_str, dv+8, 255); }
                        else { o->def_str[0]=0; o->cur_str[0]=0; }
                    } else if(strncmp(tp,"button",6)==0){
                        o->type = UOPT_BUTTON;
                    }
                    (*nopts)++;
                }
            }
            if(strncmp(buf,"uciok",5)==0){
                uci_send_raw(ei, "isready");
                int w2=0;
                while(w2<2000){
                    if(uci_recv_line(ei, buf, 512, 50)&&strncmp(buf,"readyok",7)==0) { uci_set_book(ei, use_book); return 1; }
                    w2+=50;
                }
                uci_set_book(ei, use_book);
                return 1;
            }
        } else {
            /* v12.9 FIX: only count REAL 50ms timeouts. The old code did
               "waited+=50" on every iteration, so a fast engine that streams
               its uci banner + options (Sila prints ~70 lines before uciok)
               blew the 3000ms budget after ~60 lines and uciok was never
               read -> ready=0 -> silent fallback to the built-in engine,
               i.e. "only Strong plays, Sila never moves". */
            waited+=50;
        }
    }
    return 0;
}

static void uci_close_engine(int ei){
    if(ei<0||ei>=MAX_ENGINES) return;
    if(uci_hproc[ei]){
        uci_send_raw(ei, "quit");
        WaitForSingleObject(uci_hproc[ei],600);
        TerminateProcess(uci_hproc[ei],0);
        CloseHandle(uci_hproc[ei]); uci_hproc[ei]=NULL;
        { char m[300]; snprintf(m,sizeof m,"engine closed: %s", uci_eng[ei].path); uci_dbg_log("ENGINE", ei, m); }
    }
    if(uci_hin[ei]) {CloseHandle(uci_hin[ei]); uci_hin[ei]=NULL;}
    if(uci_hout[ei]){CloseHandle(uci_hout[ei]);uci_hout[ei]=NULL;}
    uci_partial_len[ei]=0; uci_partial[ei][0]=0;
    uci_eng[ei].ready=0;
    uci_eng[ei].num_options=0;
}

#else /* ---- POSIX implementation ---- */

static int uci_send_raw(int ei, const char *s){
    if(ei<0||ei>=MAX_ENGINES||!uci_wf[ei]) return 0;
    uci_dbg_open();
    uci_dbg_log("SEND", ei, s);
    fprintf(uci_wf[ei],"%s\n",s); fflush(uci_wf[ei]); return 1;
}
static int uci_recv_line(int ei, char *buf, int maxlen, int timeout_ms){
    if(ei<0||ei>=MAX_ENGINES||!uci_rf[ei]) return 0;
    fd_set fds; FD_ZERO(&fds); FD_SET(uci_from_fd[ei],&fds);
    struct timeval tv={timeout_ms/1000,(timeout_ms%1000)*1000};
    if(select(uci_from_fd[ei]+1,&fds,NULL,NULL,&tv)<=0) return 0;
    if(!fgets(buf,maxlen,uci_rf[ei])) return 0;
    int l=strlen(buf); while(l>0&&(buf[l-1]=='\n'||buf[l-1]=='\r'))buf[--l]=0;
    uci_dbg_log("RECV", ei, buf);
    return 1;
}

static int uci_spawn_engine(int ei, const char *path){
    if(ei<0||ei>=MAX_ENGINES) return 0;
    int to_child[2],from_child[2];
    if(pipe(to_child)<0||pipe(from_child)<0) return 0;
    pid_t pid=fork();
    if(pid==0){
        dup2(to_child[0],STDIN_FILENO); dup2(from_child[1],STDOUT_FILENO);
        close(to_child[0]);close(to_child[1]);
        close(from_child[0]);close(from_child[1]);
        int dn=open("/dev/null",O_WRONLY);
        if(dn>=0){dup2(dn,STDERR_FILENO);close(dn);}
        execlp(path,path,NULL); exit(1);
    } else if(pid<0){
        close(to_child[0]);close(to_child[1]);
        close(from_child[0]);close(from_child[1]); return 0;
    }
    close(to_child[0]); close(from_child[1]);
    uci_pid[ei]=pid; uci_to_fd[ei]=to_child[1]; uci_from_fd[ei]=from_child[0];
    fcntl(uci_from_fd[ei],F_SETFL,O_NONBLOCK);
    uci_wf[ei]=fdopen(uci_to_fd[ei],"w"); uci_rf[ei]=fdopen(uci_from_fd[ei],"r");

    /* Parse UCI options during handshake */
    UCIOption *opts = uci_eng[ei].options;
    int *nopts = &uci_eng[ei].num_options;
    *nopts = 0;
    strncpy(uci_eng[ei].path, path, 255);
    uci_eng[ei].name[0]=0;

    uci_send_raw(ei, "uci");
    char buf[512]; int waited=0;
    while(waited<3000){
        if(uci_recv_line(ei, buf, 512, 50)){
            if(strncmp(buf,"id name ",8)==0 && !uci_eng[ei].name[0]){
                strncpy(uci_eng[ei].name, buf+8, 127);
                strncpy(eng_analysis[ei].name, buf+8, 127);
            }
            if(strncmp(buf,"option name ",12)==0 && *nopts < MAX_UCI_OPTIONS){
                UCIOption *o = &opts[*nopts];
                memset(o, 0, sizeof(*o));
                char *p = buf + 12;
                char *tp = strstr(p, " type ");
                if(tp){
                    int nlen = tp - p;
                    if(nlen > 127) nlen = 127;
                    memcpy(o->name, p, nlen);
                    o->name[nlen] = 0;
                    tp += 6;
                    if(strncmp(tp,"check",5)==0){
                        o->type = UOPT_CHECK;
                        char *dv = strstr(tp,"default ");
                        if(dv) o->def_check = o->cur_check = (strncmp(dv+8,"true",4)==0);
                    } else if(strncmp(tp,"spin",4)==0){
                        o->type = UOPT_SPIN;
                        char *dv = strstr(tp,"default "); if(dv) o->def_spin = o->cur_spin = atoi(dv+8);
                        char *mn = strstr(tp,"min "); if(mn) o->spin_min = atoi(mn+4);
                        char *mx = strstr(tp,"max "); if(mx) o->spin_max = atoi(mx+4);
                    } else if(strncmp(tp,"combo",5)==0){
                        o->type = UOPT_COMBO;
                        char *dv = strstr(tp,"default ");
                        char *vp = strstr(tp," var ");
                        o->combo_count = 0;
                        if(dv && (!vp || dv < vp)){
                            char *end = vp ? vp : (tp+strlen(tp));
                            int vl = end - (dv+8);
                            if(vl > 127) vl = 127;
                            memcpy(o->combo_vars[0], dv+8, vl);
                            o->combo_vars[0][vl] = 0;
                            o->combo_def_idx = o->combo_cur_idx = 0;
                            o->combo_count = 1;
                        }
                        while(vp && o->combo_count < MAX_COMBO_VARS){
                            vp += 5;
                            char *next = strstr(vp, " var ");
                            int vl = next ? (next - vp) : (int)strlen(vp);
                            if(vl > 127) vl = 127;
                            memcpy(o->combo_vars[o->combo_count], vp, vl);
                            o->combo_vars[o->combo_count][vl] = 0;
                            if(dv){
                                char defval[128];
                                char *dend = strstr(dv+8, " var ");
                                int dlen = dend ? (dend-(dv+8)) : (int)strlen(dv+8);
                                if(dlen > 127) dlen = 127;
                                memcpy(defval, dv+8, dlen);
                                defval[dlen] = 0;
                                if(strcmp(o->combo_vars[o->combo_count], defval)==0)
                                    o->combo_def_idx = o->combo_cur_idx = o->combo_count;
                            }
                            o->combo_count++;
                            vp = next;
                        }
                    } else if(strncmp(tp,"string",6)==0){
                        o->type = UOPT_STRING;
                        char *dv = strstr(tp,"default ");
                        if(dv){ strncpy(o->def_str, dv+8, 255); strncpy(o->cur_str, dv+8, 255); }
                        else { o->def_str[0]=0; o->cur_str[0]=0; }
                    } else if(strncmp(tp,"button",6)==0){
                        o->type = UOPT_BUTTON;
                    }
                    (*nopts)++;
                }
            }
            if(strncmp(buf,"uciok",5)==0){
                uci_send_raw(ei, "isready");
                int w2=0;
                while(w2<2000){
                    if(uci_recv_line(ei, buf, 512, 50)&&strncmp(buf,"readyok",7)==0) { uci_set_book(ei, use_book); return 1; }
                    w2+=50;
                }
                uci_set_book(ei, use_book);
                return 1;
            }
        } else {
            /* v12.9 FIX: only count REAL 50ms timeouts. The old code did
               "waited+=50" on every iteration, so a fast engine that streams
               its uci banner + options (Sila prints ~70 lines before uciok)
               blew the 3000ms budget after ~60 lines and uciok was never
               read -> ready=0 -> silent fallback to the built-in engine,
               i.e. "only Strong plays, Sila never moves". */
            waited+=50;
        }
    }
    return 0;
}

static void uci_close_engine(int ei){
    if(ei<0||ei>=MAX_ENGINES) return;
    if(uci_pid[ei]>0){
        if(uci_wf[ei]){fprintf(uci_wf[ei],"quit\n");fflush(uci_wf[ei]);}
        struct timeval tv={0,300000};select(0,NULL,NULL,NULL,&tv);
        kill(uci_pid[ei],SIGTERM);
        int status;waitpid(uci_pid[ei],&status,WNOHANG);
        if(uci_wf[ei])fclose(uci_wf[ei]); if(uci_rf[ei])fclose(uci_rf[ei]);
        uci_wf[ei]=NULL;uci_rf[ei]=NULL; uci_to_fd[ei]=uci_from_fd[ei]=-1; uci_pid[ei]=-1;
    }
    uci_eng[ei].ready=0;
    uci_eng[ei].num_options=0;
}
#endif /* _WIN32 / POSIX */

/* ---- Common: build UCI position string ---- */

static void uci_build_position(char *out, int maxlen){
    int pos = 0;
    /* UCI needs the literal "fen" keyword before a FEN string; sending the
       bare FEN ("position 6k1/...") makes engines silently ignore the whole
       position command and search their previous board. */
    if(game_start_fen[0]) pos += snprintf(out+pos, maxlen-pos, "position fen %s", game_start_fen);
    else                 pos += snprintf(out+pos, maxlen-pos, "position startpos");
    if(hist_n>0){
        /* v12.6 FIX: the literal "moves" keyword was missing — the command
           was "position startpos g1f3" instead of "position startpos moves
           g1f3". Strict UCI engines (Stockfish, AnMon...) silently ignore
           moves sent without the keyword, so their board stayed at the start
           position and they searched the WRONG SIDE (replied with a white
           move when it was black's turn), which the GUI then rejected as
           illegal and restarted forever. Strong/Sila tolerated the malformed
           command, which is why they worked. */
        pos += snprintf(out+pos, maxlen-pos, " moves");
        for(int i=0;i<hist_n && pos < maxlen-6; i++){
            Move *m=&hist[i].m;
            char mv[8]="";
            if(m->castle==1)strcpy(mv,"e1g1");
            else if(m->castle==2)strcpy(mv,"e1c1");
            else if(m->castle==3)strcpy(mv,"e8g8");
            else if(m->castle==4)strcpy(mv,"e8c8");
            else{
                snprintf(mv, sizeof mv, "%c%d%c%d", 'a'+m->fc, 8-m->fr, 'a'+m->tc, 8-m->tr);
                /* promo holds a piece constant (KNIGHT=2..QUEEN=5), so the
                   table needs two leading slots: "  nbrq"[promo]. The old
                   " nbrq" was off by one (knight->'b', bishop->'r', rook->'q',
                   queen->'\0'), so any promotion in the game history sent the
                   wrong letter (or none) to the engine and its board diverged
                   from the GUI's. Same table as move_to_uci_string(). */
                if(m->promo){char pm[2]={(char)("  nbrq"[m->promo]),0};strcat(mv,pm);}
            }
            pos += snprintf(out+pos, maxlen-pos, " %s", mv);
        }
    }
    out[maxlen-1] = '\0';  /* guarantee NUL termination */
}

/* v12.7: same as uci_build_position, plus one extra move at the end
   (used for the go-ponder position: the current history followed by the
   engine's predicted reply). */
static void uci_build_position_extra(char *out, int maxlen, const Move *extra){
    uci_build_position(out,maxlen);
    if(extra && extra->fr>=0){
        int pos=(int)strlen(out);
        char mv[8]="";
        if(extra->castle==1)strcpy(mv,"e1g1");
        else if(extra->castle==2)strcpy(mv,"e1c1");
        else if(extra->castle==3)strcpy(mv,"e8g8");
        else if(extra->castle==4)strcpy(mv,"e8c8");
        else{
            snprintf(mv,sizeof mv,"%c%d%c%d",'a'+extra->fc,8-extra->fr,'a'+extra->tc,8-extra->tr);
            if(extra->promo){char pm[2]={(char)("  nbrq"[extra->promo]),0};strcat(mv,pm);}
        }
        snprintf(out+pos,maxlen-pos," %s",mv);
    }
}

typedef struct { char position_cmd[8192]; int time_ms; int inc_ms; int engine_idx; } UCIArgs;

static int uci_thread_func(void *data){
    UCIArgs *a=(UCIArgs*)data;
    int ei = a->engine_idx;
    if(ei<0||ei>=MAX_ENGINES||!UCI_VALID(ei)){free(a);ai_result.fr=-1;ai_done=1;ai_thinking=0;return 0;}
    /* v12.6 FIX: start every search from a clean engine state. When a game
       or tournament is stopped (Stop tournament / new game / undo) the GUI
       cancels its own reading thread but the ENGINE keeps running the old
       "go" until its movetime elapses. A restarted tournament then queues
       ucinewgame/position/go behind that running search, and the engine's
       STALE bestmove (for the previous position) comes out of the pipe
       first — the GUI applies it instantly (that's the "moves in <1s" bug)
       or rejects it and restarts forever. Sending "stop" aborts any running
       search and "isready"/"readyok" drains the pipe, so a stale bestmove
       can never be mistaken for the answer to the new position. */
    uci_dbg_log("MOVE", ei, a->position_cmd);
    Uint32 _ov0 = SDL_GetTicks(); /* v12.10: start of administrative overhead */
    uci_send_raw(ei, "stop");
    /* Drain the engine's pending output. One isready/readyok round is not
       enough: an engine that was mid-search prints "readyok" while it is
       still unwinding and then emits the ABORTED search's bestmove AFTER
       that readyok (Strong does exactly this). A single round would leave
       that stale bestmove in the pipe and it would be read as the answer
       to the new position/go below (the "instant move" / wrong-side move
       bug after Stop -> Start). Two isready rounds guarantee the pipe is
       empty when we send the real position+go. */
    for(int _round=0; _round<2; _round++){
        uci_send_raw(ei, "isready");
        char dbuf[512];
        Uint32 d0=SDL_GetTicks();
        /* GUI FIX: was 8000ms. A PermanentBrain engine has to stop its own
           background pondering before it can answer "isready" here -- give
           it the same generous cushion as the go-search timeout below so
           this drain step isn't what quietly eats into the search budget. */
        while(SDL_GetTicks()-d0<15000 && !ai_cancel){
            if(uci_recv_line(ei, dbuf, 512, 20)){
                if(strncmp(dbuf,"readyok",7)==0) break;
            }
        }
    }
    /* v12.10: credit back whatever real time the stop+drain handshake just
       cost, so the player's clock isn't charged for GUI/engine plumbing. */
    Uint32 _ov1 = SDL_GetTicks();
    last_move_overhead_ms = (int)(_ov1 - _ov0);
    if(last_move_overhead_ms>50){ char _dbg[96]; snprintf(_dbg,sizeof _dbg,
        "stop+isready handshake took %dms -- crediting it back to the clock",
        last_move_overhead_ms); uci_dbg_log("CLOCK", ei, _dbg); }
    uci_send_raw(ei, a->position_cmd);
    char gomsg[64]; snprintf(gomsg,sizeof(gomsg),"go movetime %d",a->time_ms);
    uci_send_raw(ei, gomsg);
    char buf[1024]; uci_result.fr=-1;
    Uint32 st=SDL_GetTicks();
    /* GUI FIX: was time_ms*3+3000. Engines with their own PermanentBrain
       (Sila etc.) can legitimately take a bit longer to answer the new
       "go" than a plain engine, because they first have to cleanly unwind
       their own background pondering thread before starting the real
       search -- that unwind time is not part of "thinking time" from the
       engine's point of view, but it IS wall-clock time the GUI was
       waiting. Arena has no such tight client-side deadline; it just waits
       for bestmove. The old, tighter budget here would occasionally cut
       the wait short right as the real bestmove was about to arrive,
       logging a false "NO valid bestmove" and restarting the search under
       the SAME engine (visible in uci_debug.log as a retried "go
       movetime" for the identical position). Give a much larger cushion
       so a slower-but-legitimate PB unwind is never mistaken for a hang. */
    int timeout_ms=a->time_ms*4+15000;
    while(!ai_cancel){
        if(SDL_GetTicks()-st>(Uint32)timeout_ms) break;
        if(uci_recv_line(ei, buf, 1024, 20)){
            if(strncmp(buf,"bestmove",8)==0){
                    char mv[16]="";
                    sscanf(buf+9,"%15s",mv);
                    if(strlen(mv)>=4){
                        int fc_=mv[0]-'a',fr_=8-(mv[1]-'0');
                        int tc_=mv[2]-'a',tr_=8-(mv[3]-'0');
                        int promo_=0;
                        if(mv[4]){
                            switch(mv[4]){
                                case 'n':promo_=KNIGHT;break;case 'b':promo_=BISHOP;break;
                                case 'r':promo_=ROOK;break;  case 'q':promo_=QUEEN;break;
                            }
                        }
                        /* find matching legal move */
                        int col_idx=(turn==WHITE)?WC:BC;
                        Move all[256];int n=bb_gen_moves(&B,col_idx,all);
                        for(int i=0;i<n;i++){
                            if(all[i].fr==fr_&&all[i].fc==fc_&&all[i].tr==tr_&&all[i].tc==tc_&&
                               (!promo_||all[i].promo==promo_)){
                                BBoard bc;memcpy(&bc,&B,sizeof bc);
                                bb_do(&bc,&all[i],col_idx);
                                if(!bb_inchk(&bc,col_idx)){uci_result=all[i];break;}
                            }
                        }
                    }
                    /* v12.7: capture the engine's predicted reply "bestmove M
                       ponder R" so the GUI can run a go-ponder search on it
                       (permanent brain for external engines). R is legal in
                       the position AFTER M, not necessarily on the current
                       board, so it is stored raw and validated when the
                       opponent actually plays it. */
                    uci_last_bestmove_ponder[ei].fr = -1;
                    {
                        char *p = strchr(buf+9,' ');
                        if(p){
                            p++; while(*p==' ') p++;
                            if(strncmp(p,"ponder",6)==0){
                                p+=6; while(*p==' ') p++;
                                char pmv[16]="";
                                sscanf(p,"%15s",pmv);
                                if(strlen(pmv)>=4){
                                    Move pr;
                                    pr.fr=8-(pmv[1]-'0'); pr.fc=pmv[0]-'a';
                                    pr.tr=8-(pmv[3]-'0'); pr.tc=pmv[2]-'a';
                                    pr.promo=0; pr.cap=0; pr.castle=0; pr.ep_cap=-1; pr.score=0;
                                    if(pmv[4]){
                                        switch(pmv[4]){
                                            case 'n':pr.promo=KNIGHT;break;
                                            case 'b':pr.promo=BISHOP;break;
                                            case 'r':pr.promo=ROOK;break;
                                            case 'q':pr.promo=QUEEN;break;
                                        }
                                    }
                                    uci_last_bestmove_ponder[ei]=pr;
                                }
                            }
                        }
                    }
                    break;
            }
            /* parse info depth / score for sidebar using tokenized scan */
            if(strncmp(buf,"info",4)==0){
                    int depth=0; long long score_cp=0; int got_score=0;
                    /* Also extract PV string and mate scores */
                    int is_mate = 0;
                    long long mate_in = 0;
                    char pv_tmp[200] = "";
                    char bufcpy[1024]; strncpy(bufcpy, buf, sizeof bufcpy - 1); bufcpy[sizeof bufcpy - 1] = 0;
                    char *tok = strtok(bufcpy, " ");
                    while(tok) {
                        if(strcmp(tok, "depth") == 0) {
                            tok = strtok(NULL, " ");
                            if(tok) depth = atoi(tok);
                        } else if(strcmp(tok, "score") == 0) {
                            tok = strtok(NULL, " ");
                            if(tok) {
                                if(strcmp(tok, "cp") == 0) {
                                    tok = strtok(NULL, " ");
                                    if(tok) { score_cp = atoll(tok); got_score = 1; }
                                } else if(strcmp(tok, "mate") == 0) {
                                    tok = strtok(NULL, " ");
                                    if(tok) { mate_in = atoll(tok); is_mate = 1; got_score = 1; }
                                }
                            }
                        } else if(strcmp(tok, "nps") == 0) {
                            tok = strtok(NULL, " ");
                            if(tok) g_nps = atoll(tok);
                        } else if(strcmp(tok, "nodes") == 0) {
                            tok = strtok(NULL, " ");
                            if(tok) nodes_count = atoll(tok);
                        } else if(strcmp(tok, "pv") == 0) {
                            /* collect remaining tokens as PV */
                            char *pv_tok = strtok(NULL, " ");
                            int pv_pos = 0;
                            while(pv_tok && pv_pos < (int)sizeof(pv_tmp) - 6) {
                                pv_pos += snprintf(pv_tmp + pv_pos, sizeof(pv_tmp) - pv_pos,
                                                   "%s%s", pv_pos ? " " : "", pv_tok);
                                pv_tok = strtok(NULL, " ");
                            }
                        }
                        tok = strtok(NULL, " ");
                    }
                    if(depth > 0) g_best_depth = depth;
                    if(got_score) {
                        if(is_mate) {
                            g_best_eval = (mate_in > 0) ? (MATE - (int)mate_in) : -(MATE + (int)mate_in);
                            if(turn == BLACK) g_best_eval = -g_best_eval;
                        } else {
                            g_best_eval = (int)score_cp;
                            if(turn == BLACK) g_best_eval = -g_best_eval;
                        }
                    }
                    if(pv_tmp[0]) {
                        strncpy(g_pv_str, pv_tmp, sizeof(g_pv_str) - 1);
                        g_pv_str[sizeof(g_pv_str) - 1] = 0;
                    }
                    /* v13: mirror to per-engine panel */
                    {
                        const char *ename = uci_eng[ei].name[0] ? uci_eng[ei].name : NULL;
                        int duse = depth>0?depth:(eng_analysis[ei].has_data?eng_analysis[ei].depth:g_best_depth);
                        const char *pvuse = pv_tmp[0]?pv_tmp:(eng_analysis[ei].has_data?NULL:g_pv_str);
                        eng_analysis_update(ei, ename, duse, g_best_eval, pvuse ? pvuse : eng_analysis[ei].pv, g_nps, nodes_count, 1);
                        if(pv_tmp[0]){ strncpy(eng_analysis[ei].pv, pv_tmp, 511); eng_analysis[ei].pv[511]=0; }
                    }
            }
        }
    }
    free(a);
    ai_result=uci_result;
    /* v13: mark engine not thinking */
    if(ei>=0&&ei<2) eng_analysis[ei].is_thinking=0;
    if(ai_result.fr<0){
        uci_dbg_log("WARN", ei, "NO valid bestmove (timeout or illegal) -> fr=-1");
    }
    uci_last_mover_ei = ei; /* v12.7 */
    ai_done=1;ai_thinking=0;
    return 0;
}

static void start_uci_ai_move(int ei){
    UCIArgs *a=malloc(sizeof(UCIArgs));
    if(!a){ai_result.fr=-1;ai_done=1;ai_thinking=0;return;}
    /* v12.7: never let a ponder reader and a real search read the SAME
       pipe at the same time. Only cancel a ponder when it belongs to the
       engine about to search (in the aivsai flow the opponent's ponder
       keeps running on its own pipe while this engine searches). */
    if(uci_ponder_ei==ei) cancel_uci_ponder();
    uci_last_bestmove_ponder[ei].fr=-1; /* v12.7: fresh search, no old prediction */
    uci_build_position(a->position_cmd,sizeof(a->position_cmd));
    int my_time=(turn==WHITE)?(int)clk_w:(int)clk_b;
    a->time_ms=(my_time/40+increment);
    if(a->time_ms<50)a->time_ms=50;
    if(a->time_ms>(int)(my_time/4))a->time_ms=my_time/4;
    { char _dbg[160]; snprintf(_dbg,sizeof _dbg,
        "start_uci_ai_move: turn=%s clk_w=%u clk_b=%u base_time=%u my_time=%d -> movetime=%d",
        turn==WHITE?"W":"B", clk_w, clk_b, base_time, my_time, a->time_ms);
      uci_dbg_log("CLOCK", ei, _dbg); }
    a->inc_ms=increment;
    a->engine_idx=ei;
    /* v13: mark per-engine thinking */
    eng_analysis[ei].is_thinking=1;
    ai_thinking=1; ai_done=0;
    if(!eng_analysis[ei].has_data && uci_eng[ei].name[0]){ strncpy(eng_analysis[ei].name, uci_eng[ei].name,127); }
    /* v10 FIX: Do NOT send ucinewgame here — it clears the TT, destroying all
       ponder/PB analysis from the previous move. The engine would start each
       move from depth 1 with an empty TT instead of continuing from where
       ponder left off. ucinewgame is only sent on actual new game. */
    SDL_CreateThread(uci_thread_func,"UCI",a);
}

/* ===================== v12.7: UCI PONDER (permanent brain for external engines) =====================
   Arena-style UCI ponder: after an engine plays a move and announces
   "bestmove M ponder R", the GUI starts a "go ponder" search on the
   position after M R while the opponent thinks. If the opponent plays R,
   the GUI sends ponderhit (or reuses the already-finished result) and the
   engine answers INSTANTLY — this is what gives engines with their own
   permanent brain (Sila and friends) instant replies in our GUI, exactly
   like in Arena. If the opponent plays something else, the ponder is
   aborted with "stop" and the normal search runs; its isready drain
   already flushes any stale bestmove the aborted ponder may emit. */
static int uci_ponder_thread_func(void *data){
    int ei=(int)(intptr_t)data;
    if(ei<0||ei>=MAX_ENGINES||!UCI_VALID(ei)){uci_ponder_alive=0;return 0;}
    uci_ponder_best_set=0; uci_ponder_best.fr=-1;
    uci_dbg_log("PONDER",ei,uci_ponder_pos);
    uci_send_raw(ei,uci_ponder_pos);
    uci_send_raw(ei,uci_ponder_go);
    Uint32 hit_t0=0;
    char buf[1024];
    while(!uci_ponder_cancel){
        if(uci_ponder_waiting){
            if(hit_t0==0) hit_t0=SDL_GetTicks();
            /* v13.1: budget-scaled fallback (mirrors the normal-search wait
               budget time_ms*4+15000). The old fixed 20s fired both too late
               for short controls and too early for long ones. */
            if(SDL_GetTicks()-hit_t0>(Uint32)(uci_ponder_budget_ms*4+15000)){
                /* engine did not answer the ponderhit: fall back to a normal search */
                uci_dbg_log("PONDER",ei,"ponderhit timeout -> normal search");
                ai_result.fr=-1; ai_done=1; ai_thinking=0;
                uci_ponder_waiting=0; uci_ponder_ei=-1; uci_ponder_best_set=0;
                break;
            }
        }
        if(uci_recv_line(ei,buf,1024,50)){
            if(strncmp(buf,"bestmove",8)==0){
                char mv[16]=""; char *t=buf+9; while(*t==' ')t++;
                sscanf(t,"%15s",mv);
                Move m; m.fr=-1;
                if(strlen(mv)>=4){
                    m.fr=8-(mv[1]-'0'); m.fc=mv[0]-'a'; m.tr=8-(mv[3]-'0'); m.tc=mv[2]-'a';
                    m.promo=0; m.cap=0; m.castle=0; m.ep_cap=-1; m.score=0;
                    if(mv[4]){
                        switch(mv[4]){
                            case 'n':m.promo=KNIGHT;break; case 'b':m.promo=BISHOP;break;
                            case 'r':m.promo=ROOK;break;   case 'q':m.promo=QUEEN;break;
                        }
                    }
                }
                /* capture the new predicted reply so the PB chain continues */
                uci_last_bestmove_ponder[ei].fr=-1;
                {
                    char *p=strchr(buf+9,' ');
                    if(p){
                        p++; while(*p==' ')p++;
                        if(strncmp(p,"ponder",6)==0){
                            p+=6; while(*p==' ')p++;
                            char pmv[16]=""; sscanf(p,"%15s",pmv);
                            if(strlen(pmv)>=4){
                                Move pr; pr.fr=8-(pmv[1]-'0'); pr.fc=pmv[0]-'a';
                                pr.tr=8-(pmv[3]-'0'); pr.tc=pmv[2]-'a';
                                pr.promo=0; pr.cap=0; pr.castle=0; pr.ep_cap=-1; pr.score=0;
                                if(pmv[4]){
                                    switch(pmv[4]){
                                        case 'n':pr.promo=KNIGHT;break; case 'b':pr.promo=BISHOP;break;
                                        case 'r':pr.promo=ROOK;break;   case 'q':pr.promo=QUEEN;break;
                                    }
                                }
                                uci_last_bestmove_ponder[ei]=pr;
                            }
                        }
                    }
                }
                if(uci_ponder_waiting){
                    /* ponderhit path: this bestmove is the engine's real move
                       for the CURRENT position (opponent played the reply). */
                    int col=(turn==WHITE)?WC:BC;
                    Move all[256];int n=bb_gen_moves(&B,col,all);
                    Move ok; ok.fr=-1;
                    for(int i=0;i<n;i++){
                        if(all[i].fr==m.fr&&all[i].fc==m.fc&&all[i].tr==m.tr&&all[i].tc==m.tc&&
                           (!m.promo||all[i].promo==m.promo)){
                            BBoard bc;memcpy(&bc,&B,sizeof bc);
                            bb_do(&bc,&all[i],col);
                            if(!bb_inchk(&bc,col)){ok=all[i];break;}
                        }
                    }
                    ai_result=ok; ai_done=1; ai_thinking=0;
                    uci_ponder_waiting=0; uci_ponder_ei=-1; uci_ponder_best_set=0;
                    uci_last_mover_ei=ei;
                    uci_dbg_log("PONDER-BEST",ei,mv);
                } else {
                    /* the engine finished its ponder before the opponent moved */
                    uci_ponder_best=m; uci_ponder_best_set=1;
                    uci_dbg_log("PONDER-EARLY",ei,mv);
                }
                break;
            }
        }
    }
    uci_ponder_alive=0;
    return 0;
}

static void start_uci_ponder(int ei){
    if(!use_ponder) return; /* GUI Ponder OFF — never start external ponder */
    if(ei<0||ei>=MAX_ENGINES||!uci_eng[ei].ready||!UCI_VALID(ei)) return;
    Move R=uci_last_bestmove_ponder[ei];
    if(R.fr<0) return; /* engine announced no predicted reply */
    if(game_over||analysis_mode) return;
    if(uci_ponder_ei>=0||uci_ponder_waiting) return;
    /* v12.8: a ponder result was just consumed — its thread may still be
       winding down (it exits within ~50ms after its read times out). Give
       it a moment instead of skipping the next ponder: without this the PB
       chain would randomly break after every hit (next ponder skipped). */
    int _w=0;
    while(uci_ponder_alive && _w<60){ SDL_Delay(2); _w++; }
    if(uci_ponder_alive) return;
    uci_build_position_extra(uci_ponder_pos,sizeof(uci_ponder_pos),&R);
    /* v13.1: Arena-style time control on the ponder search. A bare "go ponder"
       carries no limit, so after "ponderhit" engines like Stockfish searched
       forever: every ponder HIT burned the ponderhit-timeout plus a full fresh
       search — ponder ON played far slower than ponder OFF or Arena. Mirror
       the normal-search budget from start_uci_ai_move here. */
    int p_eng_white=(turn==BLACK); /* turn already flipped to the opponent */
    int p_my=p_eng_white?(int)clk_w:(int)clk_b;
    int p_bud=p_my/40+(int)increment;
    if(p_bud<50)p_bud=50;
    if(p_bud>p_my/4)p_bud=p_my/4;
    uci_ponder_budget_ms=p_bud;
    snprintf(uci_ponder_go,sizeof(uci_ponder_go),
        "go ponder wtime %u btime %u winc %u binc %u movetime %d",
        clk_w,clk_b,increment,increment,p_bud);
    uci_ponder_predicted=R;
    uci_ponder_ei=ei;
    uci_ponder_cancel=0;
    uci_ponder_best_set=0; uci_ponder_best.fr=-1;
    uci_ponder_waiting=0;
    uci_ponder_alive=1;
    uci_last_bestmove_ponder[ei].fr=-1; /* consumed */
    SDL_CreateThread(uci_ponder_thread_func,"UCIponder",(void*)(intptr_t)ei);
}

static void cancel_uci_ponder(void){
    if(uci_ponder_ei<0 && !uci_ponder_alive) return;
    uci_ponder_cancel=1;
    int pe=uci_ponder_ei;
    if(pe>=0&&pe<MAX_ENGINES&&uci_eng[pe].ready&&UCI_VALID(pe)) uci_send_raw(pe,"stop");
    int wait=0;
    while(uci_ponder_alive&&wait<400){SDL_Delay(5);wait++;}
    uci_ponder_ei=-1;
    uci_ponder_waiting=0;
    uci_ponder_best_set=0;
    uci_ponder_cancel=0;
}

static void uci_ponder_opponent_moved(const Move *m){
    if(game_over||analysis_mode){cancel_uci_ponder();return;}
    if(uci_ponder_ei<0) return;
    int ei=uci_ponder_ei;
    if(m && m->fr>=0 && m->fr==uci_ponder_predicted.fr && m->fc==uci_ponder_predicted.fc &&
       m->tr==uci_ponder_predicted.tr && m->tc==uci_ponder_predicted.tc &&
       m->promo==uci_ponder_predicted.promo){
        /* hit: the opponent played the predicted reply */
        if(uci_ponder_best_set && uci_ponder_best.fr>=0){
            /* the engine already finished its ponder: reuse the result instantly */
            uci_dbg_log("PONDER-HIT",ei,"instant reuse");
            uci_ponder_ei=-1; uci_ponder_waiting=0; uci_ponder_best_set=0; uci_ponder_cancel=1;
            uci_last_mover_ei=ei;
            ai_result=uci_ponder_best; ai_done=1; ai_thinking=0;
        } else if(uci_ponder_alive){
            /* the engine is still pondering: turn it into the real search */
            uci_dbg_log("PONDER-HIT",ei,"ponderhit");
            uci_ponder_waiting=1;
            uci_send_raw(ei,"ponderhit");
        } else {
            uci_dbg_log("PONDER-HIT",ei,"no result -> normal search");
            uci_ponder_ei=-1; uci_ponder_waiting=0;
        }
    } else {
        /* miss: the opponent played something else */
        uci_dbg_log("PONDER-MISS",ei,"stop");
        cancel_uci_ponder();
    }
}

/* ===================== v10: TOURNAMENT SUBSYSTEM ===================== */
static void tourney_stop(void){
    stop_analysis();stop_pondering();stop_ai();
    for(int _ei=0;_ei<MAX_ENGINES;_ei++)
        if(uci_eng[_ei].ready && UCI_VALID(_ei)) uci_send_raw(_ei,"stop");
    tourney_active=0;tourney_waiting=0; tourney_is_rr=0; both_human=0;
    if(aivsai){ aivsai=0; player_color=turn; }
    if(tourney_is_rr) snprintf(msg,sizeof(msg),"RR Tournament stopped.");
    else snprintf(msg,sizeof(msg),"Tournament stopped. E1:%.1f E2:%.1f",tourney_score[0],tourney_score[1]);
}

static void tourney_begin_game(void){
    pgn_replay_mode=0; pgn_replay_auto=0;
    stop_analysis();stop_pondering();stop_ai();
    init_board();turn=WHITE;game_start_fen[0]=0;
    game_over=0;draw_offered=0;

    /* v10 FIX: Send ucinewgame to all connected UCI engines at game start.
       v12.10: Also disable engine's own PB/Ponder so the GUI can manage
       pondering cleanly via its own UCI Ponder mechanism. */
    for(int ei=0;ei<MAX_ENGINES;ei++){
        uci_send_raw(ei, "ucinewgame");
        uci_disable_engine_pb(ei);
    }

    /* Set up player mode based on tourney_player[] */
    int w=tourney_player[0], b=tourney_player[1];
    both_human = (w==3 && b==3);
    if(w==3){         /* white is human */
        aivsai=0; player_color=WHITE;
    } else if(b==3){  /* black is human */
        aivsai=0; player_color=BLACK;
    } else {          /* both engines */
        aivsai=1; player_color=WHITE; /* player_color unused in aivsai */
    }

    int gn=tourney_played+1;
    sprintf(msg,"Tournament game %d/%d",gn,tourney_total);

    /* Kick off the first engine move if white is an engine (not human) */
    if(w!=3) start_ai_move();
}

/* v13: if a side is still the built-in engine (0) but a real UCI engine is
   loaded and ready, promote it so a freshly started tournament actually plays
   the external engines the user just loaded — instead of the built-in silently
   continuing to play. Only a side left at the built-in default is promoted; an
   explicit W: Built-in / B: Built-in choice in the Tournament menu is kept. */
static void tourney_autoset_players(void){
    int r0=uci_eng[0].ready, r1=uci_eng[1].ready;
    for(int s=0;s<2;s++){
        if(tourney_player[s]!=0) continue;
        if(s==0){
            if(r0) tourney_player[0]=1;
            else if(r1 && tourney_player[1]!=2) tourney_player[0]=2;
        } else {
            if(r1) tourney_player[1]=2;
            else if(r0 && tourney_player[0]!=1) tourney_player[1]=1;
        }
    }
}


/* v14.2: Real tournament — round robin among 3+ engines */
static void tourney_build_rr_schedule(void){
    tourney_rr_sched_len = 0;
    tourney_rr_sched_idx = 0;
    for(int i=0;i<tourney_rr_num;i++) for(int j=i+1;j<tourney_rr_num;j++){
        if(tourney_rr_sched_len+1 < 24){
            tourney_rr_sched[tourney_rr_sched_len][0] = tourney_rr_players[i];
            tourney_rr_sched[tourney_rr_sched_len][1] = tourney_rr_players[j];
            tourney_rr_sched_len++;
            tourney_rr_sched[tourney_rr_sched_len][0] = tourney_rr_players[j];
            tourney_rr_sched[tourney_rr_sched_len][1] = tourney_rr_players[i];
            tourney_rr_sched_len++;
        }
    }
    tourney_total = tourney_rr_sched_len;
    tourney_played = 0;
    for(int i=0;i<tourney_rr_num;i++) tourney_rr_score[i]=0;
    tourney_score[0]=tourney_score[1]=0;
}
static void tourney_start_round_robin(int mode){
    // mode 3 = exactly 3 engines (built-in + 2 UCI if ready), 0 = all ready
    int players[4]; int n=0;
    // built-in always available
    players[n++]=0;
    if(uci_eng[0].ready) players[n++]=1;
    if(uci_eng[1].ready) players[n++]=2;
    // if wants 3 and we have <3, just use what we have
    if(mode==3){
        if(n<2){ bottom_log_push("RR: need at least 2 engines"); return; }
        // keep n as is (2 or 3)
    } else {
        // all ready: already n
        if(n<2){ bottom_log_push("RR: need at least 2 engines"); return; }
    }
    tourney_is_rr = 1;
    tourney_rr_num = n;
    for(int i=0;i<n;i++) tourney_rr_players[i]=players[i];
    tourney_build_rr_schedule();
    pgn_replay_mode=0; pgn_replay_auto=0;
    tourney_active=1; tourney_waiting=0;
    char msg2[128]; snprintf(msg2,sizeof(msg2),"Round Robin %d players, %d games", n, tourney_rr_sched_len);
    bottom_log_push(msg2);
    uci_dbg_log("TOURN", -1, msg2);
    // setup first game
    tourney_player[0]=tourney_rr_sched[0][0];
    tourney_player[1]=tourney_rr_sched[0][1];
    tourney_e1_engine=tourney_rr_players[0];
    tourney_begin_game();
}
static void tourney_start_now(void){
    pgn_replay_mode=0; pgn_replay_auto=0;
    tourney_autoset_players();
    tourney_active=1;tourney_played=0;
    tourney_score[0]=tourney_score[1]=0;
    tourney_waiting=0;
    tourney_e1_engine=tourney_player[0]; /* remember who E1 is */
    char _tpmsg[64]; snprintf(_tpmsg,sizeof _tpmsg,"Tournament: W=%d B=%d (0=builtin,1=E1,2=E2)",
        tourney_player[0], tourney_player[1]);
    uci_dbg_log("TOURN", -1, _tpmsg);
    tourney_begin_game();
}

/* ===================== v15: TOURNAMENT MANAGER — implementation ===================== */

/* derive a short display name from an engine path, e.g. ".../stockfish17.exe" -> "stockfish17" */
static void tm_name_from_path(const char *path, char *out, size_t outsz){
    const char *base = strrchr(path,'/'); if(!base) base = strrchr(path,'\\');
    base = base ? base+1 : path;
    strncpy(out, base, outsz-1); out[outsz-1]=0;
    char *dot = strrchr(out,'.');
    if(dot && dot!=out) *dot=0;
}

static int tm_roster_add_builtin(void){
    if(roster_n>=MAX_ROSTER) return -1;
    RosterPlayer *r=&roster[roster_n];
    memset(r,0,sizeof *r);
    strncpy(r->name,"Built-in (StrongEngine)",sizeof r->name-1);
    r->is_builtin=1; r->slot_hint=-1;
    return roster_n++;
}
static int tm_roster_add_path(const char *path){
    if(roster_n>=MAX_ROSTER || !path || !path[0]) return -1;
    RosterPlayer *r=&roster[roster_n];
    memset(r,0,sizeof *r);
    strncpy(r->path,path,sizeof r->path-1);
    tm_name_from_path(path,r->name,sizeof r->name);
    r->is_builtin=0; r->slot_hint=-1;
    return roster_n++;
}
/* v16: scan a whole folder for UCI engine executables and add all of them
   to the roster in one go, instead of typing each path in one at a time.
   Windows: any *.exe in the folder. Linux/macOS: any regular file that
   already has its executable bit set (that's what uci_spawn_engine needs
   to run it anyway, so this matches "engines I could actually launch").
   *out_found is set to how many candidate files were seen, so the caller
   can tell the user if the roster filled up before all of them fit. */
static int tm_roster_add_folder(const char *dir, int *out_found){
    char candidates[64][512];
    int n=0;
    if(out_found) *out_found=0;
    if(!dir || !dir[0]) return 0;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern,sizeof pattern,"%s\\*.exe",dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern,&fd);
    if(h!=INVALID_HANDLE_VALUE){
        do{
            if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if(n<(int)(sizeof(candidates)/sizeof(candidates[0])))
                snprintf(candidates[n++],sizeof candidates[0],"%s\\%s",dir,fd.cFileName);
        } while(FindNextFileA(h,&fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if(d){
        struct dirent *de;
        while((de=readdir(d))){
            if(de->d_name[0]=='.') continue; /* skip ., .., and hidden files */
            char full[512];
            snprintf(full,sizeof full,"%s/%s",dir,de->d_name);
            struct stat st;
            if(stat(full,&st)!=0) continue;
            if(!S_ISREG(st.st_mode)) continue;
            if(!(st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))) continue; /* must be runnable */
            if(n<(int)(sizeof(candidates)/sizeof(candidates[0])))
                strncpy(candidates[n++],full,sizeof candidates[0]-1);
        }
        closedir(d);
    }
#endif
    /* alphabetical order so the roster order is predictable */
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(strcmp(candidates[j],candidates[i])<0){
        char tmp[512]; strcpy(tmp,candidates[i]); strcpy(candidates[i],candidates[j]); strcpy(candidates[j],tmp);
    }
    if(out_found) *out_found=n;
    int added=0;
    for(int i=0;i<n;i++){
        if(tm_roster_add_path(candidates[i])>=0) added++;
        else break; /* roster full — stop, caller reports found vs added */
    }
    return added;
}
static void tm_roster_remove(int idx){
    if(idx<0||idx>=roster_n||tm_active) return;
    for(int i=idx;i<roster_n-1;i++) roster[i]=roster[i+1];
    roster_n--;
    if(tm_roster_scroll>0 && tm_roster_scroll>=roster_n) tm_roster_scroll--;
}

static void tm_reset_stats(void){
    for(int i=0;i<roster_n;i++){
        roster[i].games=roster[i].wins=roster[i].draws=roster[i].losses=0;
        roster[i].points=0; roster[i].sb=0;
    }
    memset(tm_crosstable,0,sizeof tm_crosstable);
    memset(tm_crosstable_games,0,sizeof tm_crosstable_games);
}

/* build a (optionally double) round robin: every unordered pair plays with A as
   white, repeated tm_games_per_pairing times (colors alternating each repeat);
   if tm_double_rr is set the mirrored pairing (B as white) is added right after,
   so equal playing time as both colors is guaranteed for every engine. */
static void tm_build_schedule(void){
    tm_sched_len=0; tm_sched_idx=0;
    for(int i=0;i<roster_n && tm_sched_len<MAX_TM_PAIRINGS;i++){
        for(int j=i+1;j<roster_n && tm_sched_len<MAX_TM_PAIRINGS;j++){
            for(int g=0; g<tm_games_per_pairing && tm_sched_len<MAX_TM_PAIRINGS; g++){
                int a=i,b=j;
                if(g&1){ a=j; b=i; } /* alternate colors across repeats */
                tm_sched[tm_sched_len].a=a; tm_sched[tm_sched_len].b=b; tm_sched_len++;
                if(tm_double_rr && tm_sched_len<MAX_TM_PAIRINGS){
                    tm_sched[tm_sched_len].a=b; tm_sched[tm_sched_len].b=a; tm_sched_len++;
                }
            }
        }
    }
}

/* make sure roster[ridx] (a non-builtin engine) is the process running in
   physical UCI slot `slot`; respawns only if a different engine is loaded */
static int tm_ensure_slot(int slot, int ridx){
    RosterPlayer *r=&roster[ridx];
    if(r->is_builtin) return 1;
    if(uci_eng[slot].ready && strcmp(uci_eng[slot].path,r->path)==0) { r->slot_hint=slot; return 1; }
    uci_close_engine(slot);
    if(uci_spawn_engine(slot, r->path)){
        strncpy(uci_eng[slot].path,r->path,255);
        uci_eng[slot].ready=1;
        r->slot_hint=slot;
        char lm[300]; snprintf(lm,sizeof lm,"TM: loaded '%s' into slot %d",r->name,slot);
        bottom_log_push(lm);
        return 1;
    }
    uci_eng[slot].ready=0;
    char lm[300]; snprintf(lm,sizeof lm,"TM: FAILED to load '%s' (%s)",r->name,r->path);
    bottom_log_push(lm); strncpy(msg,lm,sizeof(msg)-1);
    return 0;
}

static void tm_start_game(int idx){
    int a=tm_sched[idx].a, b=tm_sched[idx].b;
    int ok=1;
    /* v18 FIX: TM has its own "Time per game" setting; without this the clocks
       used the last value set in the main Settings menu (usually the 5:00
       default = base_time initialised at startup) no matter what was chosen
       here, because tourney_begin_game() -> init_board() just reads base_time. */
    base_time = (Uint32)tm_time_sec*1000; increment = 0;
    if(roster[a].is_builtin) tourney_player[0]=0;
    else { ok &= tm_ensure_slot(0,a); tourney_player[0]=1; }
    if(roster[b].is_builtin) tourney_player[1]=0;
    else { ok &= tm_ensure_slot(1,b); tourney_player[1]=2; }
    tm_cur_a=a; tm_cur_b=b;
    if(!ok){ tm_stop(); return; }
    tourney_begin_game();
}

static void tm_compute_sb(void){
    for(int i=0;i<roster_n;i++){
        double sb=0;
        for(int j=0;j<roster_n;j++){
            if(j==i) continue;
            sb += tm_crosstable[i][j] * roster[j].points;
        }
        roster[i].sb=sb;
    }
}

/* sorted order (by points desc, then SB desc) into out[]; returns roster_n */
static int tm_standings_order(int *out){
    for(int i=0;i<roster_n;i++) out[i]=i;
    for(int i=0;i<roster_n;i++) for(int k=i+1;k<roster_n;k++){
        int x=out[i], y=out[k];
        if(roster[y].points>roster[x].points ||
          (roster[y].points==roster[x].points && roster[y].sb>roster[x].sb)){
            out[i]=y; out[k]=x;
        }
    }
    return roster_n;
}

static void tm_export_standings(void){
    FILE *f=fopen("tournament_standings.txt","w");
    if(!f){ bottom_log_push("TM: export failed (can't open file)"); return; }
    time_t tt=time(NULL);
    fprintf(f,"Ferz Tournament Manager — standings (%s)\n", ctime(&tt));
    fprintf(f,"Games: %d/%d   Double-RR: %s   Games/pairing: %d\n\n",
        tm_sched_idx, tm_sched_len, tm_double_rr?"yes":"no", tm_games_per_pairing);
    int order[MAX_ROSTER]; tm_standings_order(order);
    fprintf(f,"%-4s %-28s %6s %5s %4s %4s %4s %7s\n","Rank","Engine","Points","Gm","W","D","L","SB");
    for(int k=0;k<roster_n;k++){
        int i=order[k];
        fprintf(f,"%-4d %-28s %6.1f %5d %4d %4d %4d %7.2f\n",
            k+1, roster[i].name, roster[i].points, roster[i].games,
            roster[i].wins, roster[i].draws, roster[i].losses, roster[i].sb);
    }
    fprintf(f,"\nCrosstable (row's score vs column):\n%-16s","");
    for(int j=0;j<roster_n;j++) fprintf(f,"%6.6s ", roster[j].name);
    fprintf(f,"\n");
    for(int i=0;i<roster_n;i++){
        fprintf(f,"%-16.16s", roster[i].name);
        for(int j=0;j<roster_n;j++){
            if(i==j) fprintf(f,"   -   ");
            else fprintf(f,"%3.1f/%-2d ", tm_crosstable[i][j], tm_crosstable_games[i][j]);
        }
        fprintf(f,"\n");
    }
    fclose(f);
    bottom_log_push("TM: standings exported to tournament_standings.txt");
    snprintf(msg,sizeof msg,"Standings exported to tournament_standings.txt");
}

static void tm_start(void){
    if(roster_n<2){ bottom_log_push("TM: need at least 2 engines in the roster"); return; }
    /* v18 FIX: if an old-style Tournament / AI-vs-AI game (or analysis/pondering)
       is still running when the user opens the manager and hits Start, TM used
       to hijack UCI engine slots 0/1 (closing+respawning them) while that game
       was still mid-"go". That killed the engine the old game was waiting on,
       so its clock kept ticking down toward a move that would never arrive,
       and tm_ensure_slot's respawn could itself race and fail, which made
       tm_start_game() bail out via tm_stop() -> "Tournament stopped" with no
       game ever starting. Always fully stop whatever was running first. */
    tourney_stop();
    pgn_replay_mode=0; pgn_replay_auto=0;
    tm_build_schedule();
    if(tm_sched_len<1){ bottom_log_push("TM: empty schedule"); return; }
    tm_reset_stats();
    tm_active=1; tourney_active=1; tourney_is_rr=0; tourney_waiting=0;
    tourney_total=tm_sched_len; tourney_played=0;
    char m2[96]; snprintf(m2,sizeof m2,"Tournament Manager: %d engines, %d games", roster_n, tm_sched_len);
    bottom_log_push(m2); uci_dbg_log("TOURN",-1,m2);
    tm_start_game(0);
}

static void tm_stop(void){
    int was_active = tm_active;
    tm_active=0;
    tourney_stop(); /* stops threads/analysis/pondering and clears tourney_active */
    if(was_active) bottom_log_push("TM: tournament stopped");
}

/* called from tourney_record_result() once per finished game while a v15
   roster tournament is running — reuses the same White/Black-wins message
   parsing convention as the legacy tournament code below. */
static void tm_record_result(void){
    float ws=0,bs=0;
    if(strstr(msg,"White wins")||strstr(msg,"white wins")||(strstr(msg,"checkmate")&&turn==BLACK)) ws=1;
    else if(strstr(msg,"Black wins")||strstr(msg,"black wins")||(strstr(msg,"checkmate")&&turn==WHITE)) bs=1;
    else if(strstr(msg,"Time!")&&strstr(msg,"White wins")) ws=1;
    else if(strstr(msg,"Time!")&&strstr(msg,"Black wins")) bs=1;
    else { ws=0.5f; bs=0.5f; }

    int a=tm_cur_a, b=tm_cur_b; /* a=white, b=black */
    double pa = ws==1?1.0:(bs==1?0.0:0.5);
    double pb = 1.0-pa;
    if(a>=0 && b>=0){
        roster[a].games++; roster[b].games++;
        roster[a].points+=pa; roster[b].points+=pb;
        if(pa==1.0) roster[a].wins++; else if(pa==0.0) roster[a].losses++; else roster[a].draws++;
        if(pb==1.0) roster[b].wins++; else if(pb==0.0) roster[b].losses++; else roster[b].draws++;
        tm_crosstable[a][b]+=pa; tm_crosstable[b][a]+=pb;
        tm_crosstable_games[a][b]++; tm_crosstable_games[b][a]++;
    }
    tm_compute_sb();
    tourney_played++;
    tm_sched_idx++;
    if(tm_sched_idx>=tm_sched_len){
        tm_active=0; tourney_active=0; tourney_waiting=0;
        if(aivsai){ aivsai=0; player_color=turn; }
        int order[MAX_ROSTER]; tm_standings_order(order);
        char buf[256]; int off=snprintf(buf,sizeof buf,"Tournament finished! 1st: %s (%.1f pts)",
            roster_n>0?roster[order[0]].name:"?", roster_n>0?roster[order[0]].points:0.0);
        strncpy(msg,buf,sizeof(msg)-1); (void)off;
        tm_export_standings();
    } else {
        tourney_waiting=1;
        tourney_next_at=SDL_GetTicks()+tourney_delay_ms;
        snprintf(msg,sizeof msg,"Game %d/%d done — next: %s vs %s",
            tm_sched_idx, tm_sched_len,
            roster[tm_sched[tm_sched_idx].a].name, roster[tm_sched[tm_sched_idx].b].name);
    }
}

static void tourney_record_result(void){
    if(tm_active){ tm_record_result(); return; }
    if(!tourney_active) return;
    float ws=0,bs=0;
    if(strstr(msg,"White wins")||strstr(msg,"white wins")||
       (strstr(msg,"checkmate")&&turn==BLACK)){
        ws=1;
    } else if(strstr(msg,"Black wins")||strstr(msg,"black wins")||
              (strstr(msg,"checkmate")&&turn==WHITE)){
        bs=1;
    } else if(strstr(msg,"Time!")&&strstr(msg,"White wins")){
        ws=1;
    } else if(strstr(msg,"Time!")&&strstr(msg,"Black wins")){
        bs=1;
    } else {
        ws=0.5f;bs=0.5f;
    }
    if(tourney_is_rr){
        // find indices of current white/black in rr_players
        int wi=-1, bi=-1;
        for(int i=0;i<tourney_rr_num;i++){
            if(tourney_rr_players[i]==tourney_player[0]) wi=i;
            if(tourney_rr_players[i]==tourney_player[1]) bi=i;
        }
        if(ws==1.0f && wi>=0) tourney_rr_score[wi]+=1.0f;
        else if(bs==1.0f && bi>=0) tourney_rr_score[bi]+=1.0f;
        else { if(wi>=0) tourney_rr_score[wi]+=0.5f; if(bi>=0) tourney_rr_score[bi]+=0.5f; }
        // also keep E1/E2 scores for display compatibility
        tourney_score[0]+= (ws==1?1: ws==0?0:0.5);
        tourney_score[1]+= (bs==1?1: bs==0?0:0.5);
        tourney_played++;
        tourney_rr_sched_idx++;
        if(tourney_played>=tourney_total || tourney_rr_sched_idx>=tourney_rr_sched_len){
            tourney_active=0; tourney_waiting=0; tourney_is_rr=0;
            if(aivsai){ aivsai=0; player_color=turn; }
            char buf[256]; int off=snprintf(buf,sizeof(buf),"RR done! ");
            for(int i=0;i<tourney_rr_num;i++){
                const char *nm = tourney_rr_players[i]==0?"Built-in": tourney_rr_players[i]==1? (uci_eng[0].name[0]?uci_eng[0].name:"UCI1"): (uci_eng[1].name[0]?uci_eng[1].name:"UCI2");
                off+=snprintf(buf+off,sizeof(buf)-off,"%s:%.1f ", nm, tourney_rr_score[i]);
            }
            strncpy(msg,buf,sizeof(msg)-1);
        } else {
            tourney_player[0]=tourney_rr_sched[tourney_rr_sched_idx][0];
            tourney_player[1]=tourney_rr_sched[tourney_rr_sched_idx][1];
            tourney_waiting=1;
            tourney_next_at=SDL_GetTicks()+tourney_delay_ms;
            snprintf(msg,sizeof(msg),"RR Game %d/%d done — next %d vs %d soon...", tourney_played, tourney_total, tourney_player[0], tourney_player[1]);
        }
        return;
    }
    /* Assign points to E1/E2 correctly based on who is currently white/black.
       tourney_player[0] = current white, tourney_player[1] = current black.
       tourney_e1_engine = always E1's engine type. */
    float e1_pts=0, e2_pts=0;
    if(ws==1.0f){
        /* White won — does white belong to E1 or E2? */
        if(tourney_player[0]==tourney_e1_engine) e1_pts=1.0f; else e2_pts=1.0f;
    } else if(bs==1.0f){
        /* Black won — does black belong to E1 or E2? */
        if(tourney_player[1]==tourney_e1_engine) e1_pts=1.0f; else e2_pts=1.0f;
    } else {
        e1_pts=0.5f; e2_pts=0.5f; /* draw */
    }
    tourney_score[0]+=e1_pts;tourney_score[1]+=e2_pts;
    tourney_played++;
    if(tourney_played>=tourney_total){
        tourney_active=0;tourney_waiting=0;
        /* v12.4: same hand-over as tourney_stop() — leave AI-vs-AI mode */
        if(aivsai){ aivsai=0; player_color=turn; }
        snprintf(msg,sizeof(msg),"Tournament done!  E1:%.1f  E2:%.1f  (%d games)",
            tourney_score[0],tourney_score[1],tourney_total);
    } else {
        /* swap colors for next game */
        int tmp=tourney_player[0];tourney_player[0]=tourney_player[1];tourney_player[1]=tmp;
        tourney_waiting=1;
        tourney_next_at=SDL_GetTicks()+tourney_delay_ms;
        sprintf(msg,"Game %d done — E1:%.1f E2:%.1f — next game soon...",
            tourney_played,tourney_score[0],tourney_score[1]);
    }
}



/* v14.3: custom tournament games count dialog */
static void draw_custom_games_dialog(void){
    int dw=400,dh=90;
    int dx=(WIN_W-dw)/2,dy=WIN_H/2-dh/2;
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,0,0,0,180);
    SDL_Rect ov={0,0,WIN_W,WIN_H};SDL_RenderFillRect(ren,&ov);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    frect(dx,dy,dw,dh,28,28,40);orect(dx,dy,dw,dh,100,140,220);
    dtxt(dx+8,dy+8,"Number of games (1-5000):",1,200,220,255);
    frect(dx+8,dy+28,dw-16,24,14,14,22);orect(dx+8,dy+28,dw-16,24,70,110,200);
    char disp[20];
    strncpy(disp,custom_games_buf,15);disp[15]=0;
    if((SDL_GetTicks()/500)%2==0)strncat(disp,"|",sizeof(disp)-strlen(disp)-1);
    dtxt(dx+12,dy+33,disp,1,220,230,255);
    dtxt(dx+8,dy+62,"Enter = Set    Esc = Cancel",1,120,130,160);
}

/* ===================== v9: FEN DIALOG DRAWING ===================== */
static void draw_fen_dialog(void){
    int dw=560,dh=90;
    int dx=(WIN_W-dw)/2,dy=WIN_H/2-dh/2;
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,0,0,0,180);
    SDL_Rect ov={0,0,WIN_W,WIN_H};SDL_RenderFillRect(ren,&ov);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    frect(dx,dy,dw,dh,28,28,40);orect(dx,dy,dw,dh,100,140,220);
    dtxt(dx+8,dy+8,"Load FEN position:",1,200,220,255);
    frect(dx+8,dy+28,dw-16,24,14,14,22);orect(dx+8,dy+28,dw-16,24,70,110,200);
    /* cursor blink */
    char disp[260];
    strncpy(disp,fen_dialog_buf,255);
    if((SDL_GetTicks()/500)%2==0)strncat(disp,"|",260-strlen(disp)-1);
    dtxt(dx+12,dy+33,disp,1,220,230,255);
    dtxt(dx+8,dy+62,"Enter = Load    Esc = Cancel    (paste FEN string above)",1,120,130,160);
}

/* v10: engine path input dialog (Linux) */
static void draw_path_dialog(void){
    int dw=580,dh=100;
    int dx=(WIN_W-dw)/2,dy=WIN_H/2-dh/2-30;
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,0,0,0,180);
    SDL_Rect ov={0,0,WIN_W,WIN_H};SDL_RenderFillRect(ren,&ov);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    frect(dx,dy,dw,dh,28,28,40);orect(dx,dy,dw,dh,200,140,60);
    char title[80];
    if(path_dialog_mode==2) snprintf(title,sizeof(title),"%s","Folder of UCI engines (every runnable file inside gets added):");
    else if(path_dialog_mode==1) snprintf(title,sizeof(title),"%s","Engine path (full path to UCI executable):");
    else snprintf(title,sizeof(title),"Engine %d path  (full path to UCI executable):", path_dialog_engine_idx+1);
    dtxt(dx+8,dy+8,title,1,220,190,120);
    frect(dx+8,dy+28,dw-16,24,14,14,22);orect(dx+8,dy+28,dw-16,24,150,100,40);
    char disp[280];
    /* show only last ~55 chars if path is long */
    int plen=strlen(path_dialog_buf);
    char *show=path_dialog_buf;
    if(plen>55) show=path_dialog_buf+plen-55;
    snprintf(disp,sizeof(disp),"%s%s",plen>55?"...":"",show);
    if((SDL_GetTicks()/500)%2==0)strncat(disp,"|",sizeof(disp)-strlen(disp)-1);
    dtxt(dx+12,dy+33,disp,1,240,210,140);
    if(path_dialog_mode==2){
        dtxt(dx+8,dy+60,"Enter = Scan folder & add all    Esc = Cancel",1,120,130,160);
        dtxt(dx+8,dy+76,"Example:  /home/you/engines   (adds every executable file inside)",1,80,90,100);
    } else {
        dtxt(dx+8,dy+60,"Enter = Connect    Esc = Cancel",1,120,130,160);
        dtxt(dx+8,dy+76,"Example:  /usr/bin/stockfish   or   /home/you/engines/lc0",1,80,90,100);
    }
}

/* v15: Tournament Manager panel — engine roster + live standings.
   Layout: header, roster list with Remove buttons (left), settings +
   Start/Stop (right of roster), a wide standings table underneath.
   Hit-testing for this layout lives in the SDL_MOUSEBUTTONDOWN handler. */
#define TM_DW 760
#define TM_DH 560
#define TM_ROW_H 24
static void draw_tourney_manager(void){
    int dw=TM_DW;
    /* v17: dx/dy stay anchored to the full-size position even when minimized,
       so the title bar doesn't jump around the screen when it collapses. */
    int dy=(WIN_H-TM_DH)/2; if(dy<10) dy=10;
    int dx=(WIN_W-dw)/2;
    int dh = tourney_mgr_minimized ? 36 : TM_DH;
    if(!tourney_mgr_minimized){
        /* full modal dimming only when expanded — minimized, the board stays usable */
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren,0,0,0,190);
        SDL_Rect ov={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(ren,&ov);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    }
    frect(dx,dy,dw,dh,32,32,40); orect(dx,dy,dw,dh,90,90,110);
    for(int i=0;i<36;i++){ int c=28+i*12/36; SDL_SetRenderDrawColor(ren,c,c+2,c+10,255); SDL_RenderDrawLine(ren,dx,dy+i,dx+dw,dy+i); }
    orect(dx,dy,dw,36,60,60,80);
    dtxt_raw(dx+13,dy+10, tourney_mgr_minimized?"Tournament Manager  (minimized — click here to restore)":"Tournament Manager",1,255,240,200);
    /* minimize/restore button, left of the close X (v17) */
    { int cx=dx+dw-56, cy=dy+7; frect(cx,cy,22,22,55,55,70); orect(cx,cy,22,22,140,140,160);
      dtxt_raw(cx+7,cy+5, tourney_mgr_minimized?"+":"_",1,225,230,240); }
    { int cx=dx+dw-28, cy=dy+7; frect(cx,cy,22,22,70,30,30); orect(cx,cy,22,22,160,70,70); dtxt_raw(cx+7,cy+5,"X",1,255,200,200); }

    if(tourney_mgr_minimized) return; /* nothing else to draw while collapsed */

    /* ---- roster (left column) ---- */
    int rx=dx+10, ry=dy+44, rw=360, rh=200;
    dtxt_raw(rx,ry-16,"Engine roster (round-robin players):",1,180,190,210);
    orect(rx,ry,rw,rh,70,70,85);
    int visible=rh/TM_ROW_H;
    if(tm_roster_scroll>roster_n-visible) tm_roster_scroll=roster_n-visible>0?roster_n-visible:0;
    if(tm_roster_scroll<0) tm_roster_scroll=0;
    for(int row=0; row<visible && tm_roster_scroll+row<roster_n; row++){
        int i=tm_roster_scroll+row;
        int yy=ry+row*TM_ROW_H;
        if(row%2==0) frect(rx+1,yy,rw-2,TM_ROW_H-1,40,40,50); else frect(rx+1,yy,rw-2,TM_ROW_H-1,34,34,44);
        char line[80]; snprintf(line,sizeof line,"%d. %s%s", i+1, roster[i].name, roster[i].is_builtin?" [built-in]":"");
        dtxt_raw(rx+6,yy+6,line,1,220,225,235);
        /* remove button */
        int bx=rx+rw-26, by=yy+2, bs=18;
        frect(bx,by,bs,bs,70,30,30); orect(bx,by,bs,bs,150,70,70);
        dtxt_raw(bx+5,by+2,"x",1,255,190,190);
    }
    /* add buttons */
    int aby=ry+rh+8;
    frect(rx,aby,150,24,40,70,110); orect(rx,aby,150,24,90,150,210); dtxt_raw(rx+8,aby+5,"+ Add engine...",1,220,235,255);
    frect(rx+158,aby,140,24,50,70,50); orect(rx+158,aby,140,24,110,170,110); dtxt_raw(rx+166,aby+5,"+ Add Built-in",1,220,255,220);
    /* v16: point at a folder full of engines and add them all at once */
    frect(rx+306,aby,150,24,70,55,90); orect(rx+306,aby,150,24,170,120,210); dtxt_raw(rx+314,aby+5,"+ Add folder...",1,235,220,255);

    /* ---- settings (right column) ---- */
    int sx=dx+dw-360, sy=dy+44;
    dtxt_raw(sx,sy-16,"Settings:",1,180,190,210);
    { char l[64]; snprintf(l,sizeof l,"Double round-robin: %s (click to toggle)", tm_double_rr?"ON":"OFF");
      frect(sx,sy,340,24,tm_double_rr?40:34,tm_double_rr?70:34,tm_double_rr?40:34); orect(sx,sy,340,24,90,140,90);
      dtxt_raw(sx+8,sy+5,l,1,220,235,220); }
    { char l[64]; snprintf(l,sizeof l,"Games per pairing:  %d", tm_games_per_pairing);
      frect(sx,sy+30,34,24,45,45,58); orect(sx,sy+30,34,24,90,90,110); dtxt_raw(sx+13,sy+35,"-",1,255,220,180);
      frect(sx+38,sy+30,302,24,45,45,58); orect(sx+38,sy+30,302,24,90,90,110); dtxt_raw(sx+48,sy+35,l,1,220,225,235);
      dtxt_raw(sx+318,sy+35,"+",1,255,220,180); }
    { char l[64]; snprintf(l,sizeof l,"Time per game:  %d:%02d", tm_time_sec/60, tm_time_sec%60);
      frect(sx,sy+60,34,24,45,45,58); orect(sx,sy+60,34,24,90,90,110); dtxt_raw(sx+13,sy+65,"-",1,255,220,180);
      frect(sx+38,sy+60,302,24,45,45,58); orect(sx+38,sy+60,302,24,90,90,110); dtxt_raw(sx+48,sy+65,l,1,220,225,235);
      dtxt_raw(sx+318,sy+65,"+",1,255,220,180); }
    { char l[64]; snprintf(l,sizeof l,"Delay: %d ms", tourney_delay_ms);
      frect(sx,sy+90,34,24,45,45,58); orect(sx,sy+90,34,24,90,90,110); dtxt_raw(sx+13,sy+95,"-",1,255,220,180);
      frect(sx+38,sy+90,302,24,45,45,58); orect(sx+38,sy+90,302,24,90,90,110); dtxt_raw(sx+48,sy+95,l,1,220,225,235);
      dtxt_raw(sx+318,sy+95,"+",1,255,220,180); }

    if(tm_active){
        char l[64]; snprintf(l,sizeof l,"Game %d / %d — running", tm_sched_idx, tm_sched_len);
        frect(sx,sy+126,340,26,70,30,30); orect(sx,sy+126,340,26,200,90,90); dtxt_raw(sx+8,sy+132,"STOP TOURNAMENT",1,255,220,220);
        dtxt_raw(sx,sy+158,l,1,200,200,120);
        if(tm_sched_idx<tm_sched_len){
            char cur[80]; snprintf(cur,sizeof cur,"%s (W) vs %s (B)", roster[tm_sched[tm_sched_idx].a].name, roster[tm_sched[tm_sched_idx].b].name);
            dtxt_raw(sx,sy+178,cur,1,180,200,220);
        }
    } else {
        frect(sx,sy+126,340,26,30,60,30); orect(sx,sy+126,340,26,90,200,90);
        dtxt_raw(sx+8,sy+132, roster_n<2?"START (need 2+ engines)":"START TOURNAMENT",1,220,255,220);
        char l[48]; snprintf(l,sizeof l,"%d players in roster, %d games scheduled", roster_n,
            roster_n>=2? (tm_double_rr? roster_n*(roster_n-1)*tm_games_per_pairing : (roster_n*(roster_n-1)/2)*tm_games_per_pairing) : 0);
        dtxt_raw(sx,sy+158,l,1,170,180,200);
    }
    frect(sx,sy+208,166,22,45,45,58); orect(sx,sy+208,166,22,110,110,130); dtxt_raw(sx+8,sy+213,"Export standings",1,210,215,230);

    /* ---- standings table ---- */
    int tx=dx+10, ty=ry+rh+44, tw=dw-20;
    dtxt_raw(tx,ty-16,"Standings:",1,180,190,210);
    orect(tx,ty,tw,dh-(ty-dy)-14,70,70,85);
    { char hdr[100]; snprintf(hdr,sizeof hdr,"%-4s %-30s %7s %5s %4s %4s %4s %7s","#","Engine","Points","Gm","W","D","L","SB");
      dtxt_raw(tx+6,ty+4,hdr,1,150,160,180); }
    int order[MAX_ROSTER]; tm_standings_order(order);
    int trows=(dh-(ty-dy)-14-24)/TM_ROW_H;
    for(int k=0;k<roster_n && k<trows;k++){
        int i=order[k]; int yy=ty+24+k*TM_ROW_H;
        if(k%2==0) frect(tx+1,yy,tw-2,TM_ROW_H-1,38,38,48); else frect(tx+1,yy,tw-2,TM_ROW_H-1,32,32,42);
        char row[120]; snprintf(row,sizeof row,"%-4d %-30.30s %7.1f %5d %4d %4d %4d %7.2f",
            k+1, roster[i].name, roster[i].points, roster[i].games, roster[i].wins, roster[i].draws, roster[i].losses, roster[i].sb);
        int R=220,G=225,B=235; if(k==0){R=255;G=215;B=110;} else if(k==1){R=210;G=215;B=225;} else if(k==2){R=205;G=160;B=110;}
        dtxt_raw(tx+6,yy+5,row,1,R,G,B);
    }
}

/* v12.3: UCI options shown as a proper overlay window instead of a giant dropdown.
   Left-click a row to change its value (toggle/cycle/open string editor/send button);
   right-click a Spin row to step it down. Scroll wheel or the arrows to page through. */
static void draw_uci_options_dialog(void){
    int ei=options_engine;
    int dw=680,dh=480;
    int dx=(WIN_W-dw)/2,dy=(WIN_H-dh)/2;
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,0,0,0,190);
    SDL_Rect ov={0,0,WIN_W,WIN_H};SDL_RenderFillRect(ren,&ov);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    // soft shadow
    SDL_SetRenderDrawColor(ren,0,0,0,80);
    SDL_Rect sh={dx+6,dy+6,dw,dh}; SDL_RenderFillRect(ren,&sh);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    frect(dx,dy,dw,dh,32,32,40);orect(dx,dy,dw,dh,90,90,110);
    SDL_SetRenderDrawColor(ren,60,60,75,255); SDL_RenderDrawLine(ren,dx+1,dy+1,dx+dw-2,dy+1);

    /* header with gradient */
    for(int i=0;i<36;i++){
        int c = 28 + i*12/36;
        SDL_SetRenderDrawColor(ren,c,c+2,c+8,255);
        SDL_RenderDrawLine(ren,dx,dy+i,dx+dw,dy+i);
    }
    orect(dx,dy,dw,36,60,60,80);
    char ename[256]; strncpy(ename,uci_eng[ei].path[0]?uci_eng[ei].path:"(not loaded)",255); ename[255]=0;
    char *slash=strrchr(ename,'/'); char *slash2=strrchr(ename,'\\');
    if(slash2&&(!slash||slash2>slash))slash=slash2;
    if(slash) memmove(ename,slash+1,strlen(slash));
    char title[256]; snprintf(title,sizeof(title),"UCI Options — Engine %d: %s",ei+1,ename);
    // show full name if space — truncate only if extremely long to avoid overlapping tabs
    dtxt_raw(dx+14,dy+11,title,1,240,220,180);
    dtxt_raw(dx+13,dy+10,title,1,255,240,200);
    /* engine tab switcher — pill style */
    for(int t=0;t<2;t++){
        int tx=dx+dw-190+t*74,ty=dy+7,tw=68,th=22;
        int act=(t==ei);
        if(act){ frect(tx,ty,tw,th,255,165,0); orect(tx,ty,tw,th,255,200,80); }
        else { frect(tx,ty,tw,th,45,45,55); orect(tx,ty,tw,th,80,80,95); }
        char lb[16];sprintf(lb,"Engine %d",t+1);
        dtxt_raw(tx+10,ty+6,lb,1,act?20:180,act?20:180,act?20:200);
    }
    /* close X — rounded */
    {
        int cx=dx+dw-28, cy=dy+7;
        SDL_SetRenderDrawColor(ren,70,30,30,255); SDL_Rect cr={cx,cy,22,22}; SDL_RenderFillRect(ren,&cr);
        orect(cx,cy,22,22,160,70,70);
        dtxt_raw(cx+7,cy+5,"X",1,255,200,200);
    }

    int list_y=dy+42, list_h=dh-42-30;
    int n=uci_eng[ei].num_options;
    if(n==0){
        dtxt_raw(dx+12,list_y+8,"No options reported by this engine (or it isn't connected yet).",1,150,150,170);
    } else {
        int row_h=27;
        int visible=list_h/row_h; if(visible<1)visible=1;
        if(options_scroll>n-visible) options_scroll=n-visible>0?n-visible:0;
        if(options_scroll<0) options_scroll=0;
        for(int row=0; row<visible && options_scroll+row<n; row++){
            int oi=options_scroll+row;
            UCIOption *o=&uci_eng[ei].options[oi];
            int ry=list_y+row*row_h;
            // alternating row with subtle hover
            int is_hover = 0; // could add mouse hover later
            if(row%2==0) frect(dx+6,ry,dw-12,row_h-2,38,38,48);
            else frect(dx+6,ry,dw-12,row_h-2,32,32,42);
            if(is_hover) frect(dx+6,ry,dw-12,row_h-2,55,45,20);
            // icon per type
            char icon[4]=" ";
            int icR=150,icG=150,icB=160;
            if(o->type==UOPT_CHECK){ strcpy(icon, o->cur_check?"[x]":"[ ]"); icR=o->cur_check?255:120; icG=o->cur_check?165:120; icB=o->cur_check?0:120; }
            else if(o->type==UOPT_SPIN){ strcpy(icon,"#"); icR=170; icG=210; icB=230; }
            else if(o->type==UOPT_COMBO){ strcpy(icon,">"); icR=180; icG=180; icB=220; }
            else if(o->type==UOPT_STRING){ strcpy(icon,"T"); icR=200; icG=220; icB=180; }
            else if(o->type==UOPT_BUTTON){ strcpy(icon,">>"); icR=255; icG=200; icB=80; }
            char buf[220];
            switch(o->type){
                case UOPT_CHECK: snprintf(buf,sizeof(buf),"%s",o->name); break;
                case UOPT_SPIN:  snprintf(buf,sizeof(buf),"%s: %d  (range %d-%d)",o->name,o->cur_spin,o->spin_min,o->spin_max); break;
                case UOPT_COMBO: snprintf(buf,sizeof(buf),"%s: %s",o->name,o->combo_count>0?o->combo_vars[o->combo_cur_idx]:"?"); break;
                case UOPT_STRING:snprintf(buf,sizeof(buf),"%s: %s",o->name,o->cur_str[0]?o->cur_str:"<empty>"); break;
                case UOPT_BUTTON:snprintf(buf,sizeof(buf),"%s",o->name); break;
            }
            int maxch=(int)((dw-54)/RAW_ADV); if((int)strlen(buf)>maxch){buf[maxch-3]=0;strcat(buf,"...");}
            int R=220,G=220,B=230;
            if(o->type==UOPT_BUTTON){R=255;G=220;B=120;}
            else if(o->type==UOPT_SPIN){R=180;G=220;B=240;}
            else if(o->type==UOPT_CHECK && o->cur_check){R=255;G=235;B=180;}
            // по-красива отметка — истинско квадратче, не текст [x] който се слива
            if(o->type==UOPT_CHECK){
                int cbx=dx+12, cby=ry+7, cbs=13;
                orect(cbx,cby,cbs,cbs, 90,90,100);
                if(o->cur_check){
                    frect(cbx+2,cby+2,cbs-4,cbs-4, 255,165,0);
                    orect(cbx+2,cby+2,cbs-4,cbs-4, 255,200,100);
                } else {
                    frect(cbx+2,cby+2,cbs-4,cbs-4, 30,30,35);
                }
                dtxt_raw(dx+32,ry+9,buf,1,R,G,B);
            } else {
                dtxt_raw(dx+12,ry+8,icon,1,icR,icG,icB);
                dtxt_raw(dx+32,ry+9,buf,1,R,G,B);
            }
        }
        /* scroll indicators */
        if(options_scroll>0) dtxt_raw(dx+dw-16,list_y-2,"^",1,180,180,220);
        if(options_scroll+visible<n) dtxt_raw(dx+dw-16,list_y+list_h-10,"v",1,180,180,220);
    }
    dtxt_raw(dx+10,dy+dh-22,"Left-click = change value / type exact number (Spin)    Right-click = quick step down (Spin)    Wheel = scroll    Esc = close",1,120,130,160);
}

/* v10: string option input dialog */
static void draw_stropt_dialog(void){
    int dw=480,dh=100;
    int dx=(WIN_W-dw)/2,dy=WIN_H/2-dh/2-30;
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren,0,0,0,180);
    SDL_Rect ov={0,0,WIN_W,WIN_H};SDL_RenderFillRect(ren,&ov);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
    frect(dx,dy,dw,dh,28,28,40);orect(dx,dy,dw,dh,100,140,220);
    int ei = options_engine;
    char title[200];
    if(stropt_dialog_opt_idx >= 0 && stropt_dialog_opt_idx < uci_eng[ei].num_options){
        UCIOption *oo=&uci_eng[ei].options[stropt_dialog_opt_idx];
        if(stropt_is_spin) snprintf(title,sizeof(title),"Set: %s  (range %d-%d)", oo->name, oo->spin_min, oo->spin_max);
        else snprintf(title,sizeof(title),"Set: %s", oo->name);
    }
    else strcpy(title,"Set string option:");
    dtxt(dx+8,dy+8,title,1,200,220,255);
    frect(dx+8,dy+28,dw-16,24,14,14,22);orect(dx+8,dy+28,dw-16,24,70,110,200);
    char disp[280];
    int plen=strlen(stropt_dialog_buf);
    char *show=stropt_dialog_buf;
    if(plen>50) show=stropt_dialog_buf+plen-50;
    snprintf(disp,sizeof(disp),"%s%s",plen>50?"...":"",show);
    if((SDL_GetTicks()/500)%2==0)strncat(disp,"|",sizeof(disp)-strlen(disp)-1);
    dtxt(dx+12,dy+33,disp,1,220,230,255);
    dtxt(dx+8,dy+60,"Enter = Apply    Esc = Cancel",1,120,130,160);
}


/* ===================== MAIN ===================== */
int main(void){
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO);IMG_Init(IMG_INIT_PNG);
    win=SDL_CreateWindow("Chess GUI v14 — CMD Edition [CMD Black/Gray/Orange] — Dual UCI — Per-Engine Panels",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,WIN_W,WIN_H,SDL_WINDOW_RESIZABLE);
    real_w=WIN_W; real_h=WIN_H;
    ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    init_fonts(); /* v14: TTF fonts — must load before any dtxt()/dtxt_raw() call */
    load_pieces();
#ifdef USE_BOOK
    init_br();
#endif
    SDL_AudioSpec wa={0};wa.freq=44100;wa.format=AUDIO_S16;wa.channels=1;wa.samples=512;
    aud=SDL_OpenAudioDevice(NULL,0,&wa,NULL,0);if(aud)SDL_PauseAudioDevice(aud,0);

    init_lmr_table();
    init_zobrist();
    memset(transposition_table,0,sizeof(transposition_table));
    memset(pawn_tt,0,sizeof(pawn_tt));
    init_attacks();
    srand(time(NULL));
    /* v13: init per-engine analysis */
    memset(eng_analysis,0,sizeof(eng_analysis));
    strncpy(eng_analysis[ENG_BUILTIN_IDX].name,"StrongEngine (Built-in)",127);
    strncpy(eng_analysis[0].name,"UCI Engine 1",127);
    strncpy(eng_analysis[1].name,"UCI Engine 2",127);
    bottom_log_push("=== Chess GUI v14 CMD started ===");
    bottom_log_push("Board 54px — clocks above board");
    bottom_log_push("Engines: separate panels + bottom LOG tab");
    init_board();
    strcpy(msg,"Your move (White)");

    SDL_Event e;int run=1,mx=0,my=0;
    while(run){
        Uint32 now=SDL_GetTicks();Uint32 dt=now-last_ms;last_ms=now;
        uci_watchdog(); /* v13: detect engine crashes for the debug log */
        /* v12.9: clocks stay frozen until the first move is played */
        if(!game_over&&!promo_pending&&clock_started){
            if(turn==WHITE){if(clk_w>dt)clk_w-=dt;else{clk_w=0;game_over=1;if(ponder_running)stop_pondering();if(ai_thinking)stop_ai();strcpy(msg,"Time! Black wins");tourney_record_result();}}
            else           {if(clk_b>dt)clk_b-=dt;else{clk_b=0;game_over=1;if(ponder_running)stop_pondering();if(ai_thinking)stop_ai();strcpy(msg,"Time! White wins");tourney_record_result();}}
        }
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT){run=0;stop_ai();stop_pondering();break;}
            if(e.type==SDL_MOUSEMOTION){mx=e.motion.x;my=e.motion.y;if(arrow_dragging){arrow_mx=mx;arrow_my=my;}if(drag_active){drag_mx=mx;drag_my=my;}
                if(open_menu>=0){
                    int mw2=120,mx02=4,gap2=4;
                    int overBar=-1;
                    for(int mi2=0;mi2<N_MENUS;mi2++){int bx2=mx02+mi2*(mw2+gap2); if(mx>=bx2&&mx<=bx2+mw2&&my>=3&&my<=MENU_H-3) overBar=mi2;}
                    if(overBar>=0 && overBar!=open_menu){ open_menu=overBar; menu_outside_t0=0; }
                    else{
                        int mi=open_menu,n=menu_count(mi),ih=26,iw=(mi==5)?320:220,ix=mx02+mi*(mw2+gap2),iy=MENU_H;
                        if(ix+iw>WIN_W) ix=WIN_W-iw-2;
                        int overDrop=(mx>=ix&&mx<ix+iw&&my>=iy&&my<iy+4+n*ih);
                        /* v13.1: 250ms grace before auto-closing — without it a
                           single motion event outside the rect (e.g. while the
                           mouse travels from the bar to the item) killed the
                           menu before the click could land. */
                        if(overBar==open_menu||overDrop) menu_outside_t0=0;
                        else{
                            Uint32 mnow=SDL_GetTicks();
                            if(menu_outside_t0==0) menu_outside_t0=mnow;
                            else if(mnow-menu_outside_t0>250){ open_menu=-1; menu_outside_t0=0; }
                        }
                    }
                }
            }
            if(e.type==SDL_KEYDOWN){
                SDL_Keycode k=e.key.keysym.sym;int ctrl=(e.key.keysym.mod&KMOD_CTRL);
                /* v12.3: UCI options window keyboard */
                if(uci_opts_dialog_active && !stropt_dialog_active){
                    if(k==SDLK_ESCAPE){uci_opts_dialog_active=0;}
                    goto skip_normal_keys;
                }
                /* v15: Tournament Manager panel keyboard */
                if(tourney_mgr_active && !path_dialog_active){
                    if(k==SDLK_ESCAPE){tourney_mgr_active=0;tourney_mgr_minimized=0;}
                    goto skip_normal_keys;
                }
                /* v14.3: custom tournament games count dialog */
                if(custom_games_dialog_active){
                    if(k==SDLK_ESCAPE){custom_games_dialog_active=0;SDL_StopTextInput();}
                    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){
                        custom_games_dialog_active=0;SDL_StopTextInput();
                        int n=atoi(custom_games_buf);
                        if(n>=1&&n<=5000){tourney_total=n;sprintf(msg,"Tournament set to %d games",tourney_total);}
                        else{strcpy(msg,"Invalid games count (1-5000)");tourney_total=10;}
                    }
                    else if(k==SDLK_BACKSPACE&&custom_games_len>0){
                        custom_games_buf[--custom_games_len]=0;
                    }
                    goto skip_normal_keys;
                }
                /* v9: FEN dialog keyboard */
                if(fen_dialog_active){
                    if(ctrl && k==SDLK_v){
                        char *clip = SDL_GetClipboardText();
                        if(clip){
                            int avail = (int)sizeof(fen_dialog_buf) - fen_dialog_len - 1;
                            if(avail>0){
                                strncat(fen_dialog_buf, clip, avail);
                                fen_dialog_len = strlen(fen_dialog_buf);
                            }
                            SDL_free(clip);
                        }
                        goto skip_normal_keys;
                    }
                    if(k==SDLK_ESCAPE){fen_dialog_active=0;SDL_StopTextInput();}
                    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){
                        // Trim leading/trailing whitespace from paste
                        char tmp[256]; strncpy(tmp,fen_dialog_buf,255); tmp[255]=0;
                        // trim
                        int tlen=strlen(tmp); while(tlen>0 && (tmp[tlen-1]=='\n' || tmp[tlen-1]=='\r' || tmp[tlen-1]==' ' || tmp[tlen-1]=='\t')) tmp[--tlen]=0;
                        char *tstart=tmp; while(*tstart==' '||*tstart=='\t') tstart++;
                        if(strlen(tstart)>10){
                            stop_analysis();stop_pondering();stop_ai();pgn_replay_mode=0;pgn_replay_auto=0;
                            if(parse_fen(tstart)){
                                strncpy(game_start_fen,tstart,255);
                                hist_n=0;game_hist_n=0;
                                if(game_hist_n<MAX_GAME_HIST)game_hist_hashes[game_hist_n++]=B.hash;
                                clk_w=clk_b=base_time;last_ms=SDL_GetTicks();clock_started=0;
                                lm_fr=lm_fc=lm_tr=lm_tc=-1;
                                promo_pending=0;memset(cap_w,0,sizeof cap_w);memset(cap_b,0,sizeof cap_b);
                                sel_r=sel_c=-1;game_over=0;draw_offered=0;
                                strcpy(msg,"FEN loaded");
                                char log2[300]; snprintf(log2,sizeof(log2),"FEN OK: %s", tstart);
                                bottom_log_push(log2);
                            } else {
                                strcpy(msg,"Invalid FEN!");
                                char log3[300]; snprintf(log3,sizeof(log3),"FEN FAIL: '%s' len=%d", tstart, (int)strlen(tstart));
                                bottom_log_push(log3);
                            }
                        } else {
                            strcpy(msg,"FEN too short!");
                            bottom_log_push("FEN too short - paste a full FEN");
                        }
                        fen_dialog_active=0;SDL_StopTextInput();
                    }
                    else if(k==SDLK_BACKSPACE&&fen_dialog_len>0){
                        fen_dialog_buf[--fen_dialog_len]=0;
                    }
                    goto skip_normal_keys;
                }
                /* v10: engine path dialog keyboard (Linux) */
                if(path_dialog_active){
                    if(ctrl && k==SDLK_v){
                        char *clip = SDL_GetClipboardText();
                        if(clip){
                            int avail = (int)sizeof(path_dialog_buf) - path_dialog_len - 1;
                            if(avail>0){
                                strncat(path_dialog_buf, clip, avail);
                                path_dialog_len = strlen(path_dialog_buf);
                            }
                            SDL_free(clip);
                        }
                        goto skip_normal_keys;
                    }
                    if(k==SDLK_ESCAPE){path_dialog_active=0;SDL_StopTextInput();path_dialog_mode=0;}
                    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){
                        path_dialog_active=0;SDL_StopTextInput();
                        if(path_dialog_len>0){
                            if(path_dialog_mode==2){
                                /* v16: scan the whole folder and add every runnable engine found */
                                int found=0;
                                int added = tm_roster_add_folder(path_dialog_buf,&found);
                                if(found==0) snprintf(msg,sizeof msg,"No runnable engine files found in: %.60s",path_dialog_buf);
                                else if(added<found) snprintf(msg,sizeof msg,"Added %d/%d engines (roster full, max 12)",added,found);
                                else snprintf(msg,sizeof msg,"Added %d engine%s from folder",added,added==1?"":"s");
                            } else if(path_dialog_mode==1){
                                /* v15: add an engine to the tournament roster (no live spawn yet —
                                   it is only spawned into a physical slot when its game starts) */
                                int idx = tm_roster_add_path(path_dialog_buf);
                                if(idx>=0) snprintf(msg,sizeof msg,"Added to roster: %s",roster[idx].name);
                                else bottom_log_push("TM: roster is full (max "  "12" " engines)");
                            } else {
                                int ei = path_dialog_engine_idx;
                                strncpy(uci_eng[ei].path,path_dialog_buf,255);
                                uci_close_engine(ei);
                                if(uci_spawn_engine(ei, uci_eng[ei].path)){
                                    uci_eng[ei].ready=1;use_uci_engine=1;active_engine=ei;
                                    char *name=strrchr(uci_eng[ei].path,'/');
                                    sprintf(msg,"UCI Engine %d: %s",ei+1,name?name+1:uci_eng[ei].path);
                                } else {
                                    uci_eng[ei].ready=0;
                                    sprintf(msg,"UCI Engine %d failed: %s",ei+1,uci_eng[ei].path);
                                }
                            }
                        }
                        path_dialog_mode=0;
                    }
                    else if(k==SDLK_BACKSPACE&&path_dialog_len>0){
                        path_dialog_buf[--path_dialog_len]=0;
                    }
                    goto skip_normal_keys;
                }
                /* v10: string option dialog */
                if(stropt_dialog_active){
                    if(ctrl && k==SDLK_v){
                        char *clip = SDL_GetClipboardText();
                        if(clip){
                            int avail = (int)sizeof(stropt_dialog_buf) - stropt_dialog_len - 1;
                            if(avail>0){
                                strncat(stropt_dialog_buf, clip, avail);
                                stropt_dialog_len = strlen(stropt_dialog_buf);
                            }
                            SDL_free(clip);
                        }
                        goto skip_normal_keys;
                    }
                    if(k==SDLK_ESCAPE){stropt_dialog_active=0;SDL_StopTextInput();stropt_is_spin=0;}
                    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){
                        stropt_dialog_active=0;SDL_StopTextInput();
                        int ei = options_engine;
                        if(stropt_dialog_opt_idx>=0 && stropt_dialog_opt_idx<uci_eng[ei].num_options){
                            UCIOption *o = &uci_eng[ei].options[stropt_dialog_opt_idx];
                            char cmd[512];
                            if(stropt_is_spin){
                                int v = (stropt_dialog_len>0) ? atoi(stropt_dialog_buf) : o->cur_spin;
                                if(v<o->spin_min) v=o->spin_min;
                                if(v>o->spin_max) v=o->spin_max;
                                o->cur_spin = v;
                                snprintf(cmd,sizeof(cmd),"setoption name %s value %d", o->name, o->cur_spin);
                                uci_send_raw(ei, cmd);
                                sprintf(msg,"Set %s = %d", o->name, o->cur_spin);
                            } else {
                                strncpy(o->cur_str, stropt_dialog_buf, 255);
                                snprintf(cmd,sizeof(cmd),"setoption name %s value %s", o->name, o->cur_str);
                                uci_send_raw(ei, cmd);
                                sprintf(msg,"Set %s = %s", o->name, o->cur_str);
                            }
                        }
                        stropt_is_spin=0;
                    }
                    else if(k==SDLK_BACKSPACE&&stropt_dialog_len>0){
                        stropt_dialog_buf[--stropt_dialog_len]=0;
                    }
                    goto skip_normal_keys;
                }
                if(k==SDLK_r){stop_analysis();stop_pondering();stop_ai();pgn_replay_mode=0;pgn_replay_auto=0;player_color=WHITE;flip_board=0;aivsai=0;tourney_active=0;tourney_waiting=0;init_board();game_start_fen[0]=0;
                    /* v10 FIX: ucinewgame on new game */
                    for(int _ei=0;_ei<MAX_ENGINES;_ei++){uci_send_raw(_ei,"ucinewgame");uci_disable_engine_pb(_ei);}
                    strcpy(msg,"Your move (White)");}
                if(k==SDLK_u&&!ai_thinking){stop_pondering();do_undo();if(hist_n>0&&turn!=player_color)do_undo();}
                if(k==SDLK_f)flip_board=!flip_board;
                if(k==SDLK_m)sound_on=!sound_on;
                if(ctrl&&k==SDLK_s)save_pgn();
                if(k==SDLK_ESCAPE){open_menu=-1;pgn_replay_auto=0;}
                if(k==SDLK_a&&!ctrl)arrow_count=0;
                /* v9: L = load FEN */
                if(k==SDLK_l&&!ctrl){
                    fen_dialog_active=1;fen_dialog_buf[0]=0;fen_dialog_len=0;SDL_StartTextInput();
                }
                /* v9: T = toggle tournament */
                if(k==SDLK_t&&!ctrl){
                    if(tourney_active)tourney_stop();
                    else{tourney_total=10;tourney_player[0]=1;tourney_player[1]=2;tourney_start_now();}
                }
                /* Replay: arrow keys */
                if(k==SDLK_LEFT && !ctrl){
                    if(pgn_replay_mode){pgn_replay_auto=0;pgn_replay_prev();}
                    else {int rh2=real_h>0?real_h:WIN_H; if(my>=rh2-LOG_H) bottom_log_tab=(bottom_log_tab+2)%3;}
                }
                if(k==SDLK_RIGHT && !ctrl){
                    if(pgn_replay_mode){pgn_replay_auto=0;pgn_replay_next();}
                    else {int rh2=real_h>0?real_h:WIN_H; if(my>=rh2-LOG_H) bottom_log_tab=(bottom_log_tab+1)%3;}
                }
                if(k==SDLK_HOME && !ctrl){pgn_replay_auto=0;pgn_replay_first();}
                if(k==SDLK_END && !ctrl){pgn_replay_auto=0;pgn_replay_last();}
                if(k==SDLK_g && !ctrl) pgn_replay_auto_play();
                /* v8.3.1: I = toggle infinite analysis */
                if(k==SDLK_i){
                    if(analysis_mode){ stop_analysis(); strcpy(msg,turn==player_color?"Your move":"Analysis stopped"); }
                    else { start_analysis(); }
                }
                /* v13: C = copy the on-screen log to the clipboard */
                if(k==SDLK_c && !ctrl) copy_log_to_clipboard();
                /* v14: Ctrl+C = copy FEN, Y = copy FEN */
                if(ctrl && k==SDLK_c) copy_fen_to_clipboard();
                if(k==SDLK_y && !ctrl) copy_fen_to_clipboard();
                skip_normal_keys:;
            }
            /* v9: text input for FEN and path dialogs */
            if(e.type==SDL_TEXTINPUT){
                if(fen_dialog_active&&fen_dialog_len<(int)sizeof(fen_dialog_buf)-1){
                    strncat(fen_dialog_buf,e.text.text,sizeof(fen_dialog_buf)-fen_dialog_len-1);
                    fen_dialog_len=strlen(fen_dialog_buf);
                }
                if(path_dialog_active&&path_dialog_len<(int)sizeof(path_dialog_buf)-1){
                    strncat(path_dialog_buf,e.text.text,sizeof(path_dialog_buf)-path_dialog_len-1);
                    path_dialog_len=strlen(path_dialog_buf);
                }
                if(stropt_dialog_active&&stropt_dialog_len<(int)sizeof(stropt_dialog_buf)-1){
                    int ok_char = 1;
                    if(stropt_is_spin){
                        char c0 = e.text.text[0];
                        ok_char = (c0>='0'&&c0<='9') || (c0=='-'&&stropt_dialog_len==0);
                    }
                    if(ok_char){
                        strncat(stropt_dialog_buf,e.text.text,sizeof(stropt_dialog_buf)-stropt_dialog_len-1);
                        stropt_dialog_len=strlen(stropt_dialog_buf);
                    }
                }
                if(custom_games_dialog_active&&custom_games_len<(int)sizeof(custom_games_buf)-1){
                    strncat(custom_games_buf,e.text.text,sizeof(custom_games_buf)-custom_games_len-1);
                    custom_games_len=strlen(custom_games_buf);
                }
            }
            if(e.type==SDL_WINDOWEVENT){
                if(e.window.event==SDL_WINDOWEVENT_RESIZED){
                    int new_w=e.window.data1,new_h=e.window.data2;
                    real_w=new_w; real_h=new_h;
                    /* v13 fix: the height term was missing CLOCK_BAR_H (it used
                       "2*BPAD" where the vertical layout actually needs
                       "CLOCK_BAR_H+BPAD" — see BOARD_OY/FRAME_H). That made the
                       computed board size ~26px too tall whenever height was the
                       limiting dimension (typically after maximizing), pushing
                       the bottom log panel past the window edge and clipping a
                       line off it. Width term was already correct. */
                    int ns=MIN((new_w-SIDE_W-COORD_W-2*BOARD_GAP)/8,
                               (new_h-MENU_H-CLOCK_BAR_H-2*BOARD_GAP-COORD_H-LOG_H)/8);
                    SQ_SIZE=ns;
                    if(SQ_SIZE<40)SQ_SIZE=40;
                    if(SQ_SIZE>128)SQ_SIZE=128;
                }
            }
            if(e.type==SDL_MOUSEBUTTONDOWN){
                if(fen_dialog_active||path_dialog_active||stropt_dialog_active||custom_games_dialog_active) goto skip;
                // v13 CMD: bottom log tabs
                {
                    int rh = real_h>0?real_h:WIN_H;
                    int y0 = rh - LOG_H;
                    if(y0 < MENU_H+FRAME_H) y0 = MENU_H+FRAME_H; /* v13 fix: must match draw_bottom_log's
                        clamp exactly, or the click hit-box drifts away from the visually drawn tabs
                        whenever the board is large enough to push the log bar down (e.g. resized
                        window) — this was why the ENGINES/LOG tabs looked unresponsive. */
                    int my=e.button.y, mx=e.button.x;
                    if(my>=y0 && my < y0+18){
                        int tab_w=70;
                        for(int t=0;t<3;t++){
                            int tx=COORD_W + t*(tab_w+6);
                            if(mx>=tx && mx<tx+tab_w){ bottom_log_tab=t; char tmsg[48]; snprintf(tmsg,sizeof(tmsg),"TAB -> %s", t==0?"Out1":t==1?"Out2":"Log"); bottom_log_push(tmsg); goto skip; }
                        }
                    }
                }
                // v14: FEN panel click to copy (sidebar)
                {
                    int mx=e.button.x, my=e.button.y;
                    if(mx>=fen_panel_x && mx<fen_panel_x+fen_panel_w && my>=fen_panel_y && my<fen_panel_y+fen_panel_h && fen_panel_w>0){
                        copy_fen_to_clipboard();
                        goto skip;
                    }
                }
                if(uci_opts_dialog_active){
                    int cmx=e.button.x,cmy=e.button.y;
                    int dw=680,dh=480;
                    int dx=(WIN_W-dw)/2,dy=(WIN_H-dh)/2;
                    if(cmx<dx||cmx>dx+dw||cmy<dy||cmy>dy+dh){ uci_opts_dialog_active=0; goto skip; } /* click outside = close */
                    /* close X */
                    if(cmx>=dx+dw-28&&cmx<=dx+dw-6&&cmy>=dy+6&&cmy<=dy+28){ uci_opts_dialog_active=0; goto skip; }
                    /* engine tabs */
                    for(int t=0;t<2;t++){
                        int tx=dx+dw-190+t*74,ty=dy+7,tw=68,th=22;
                        if(cmx>=tx&&cmx<=tx+tw&&cmy>=ty&&cmy<=ty+th){ options_engine=t; options_scroll=0; goto skip; }
                    }
                    /* option rows */
                    int ei=options_engine;
                    int list_y=dy+42, list_h=dh-42-30, row_h=27;
                    int visible=list_h/row_h; if(visible<1)visible=1;
                    int n=uci_eng[ei].num_options;
                    if(cmy>=list_y&&cmy<list_y+visible*row_h){
                        int row=(cmy-list_y)/row_h;
                        int oi=options_scroll+row;
                        if(oi>=0&&oi<n){
                            UCIOption *o=&uci_eng[ei].options[oi];
                            char cmd[512];
                            int step;
                            switch(o->type){
                                case UOPT_CHECK:
                                    o->cur_check=!o->cur_check;
                                    snprintf(cmd,sizeof(cmd),"setoption name %s value %s",o->name,o->cur_check?"true":"false");
                                    uci_send_raw(ei,cmd);
                                    break;
                                case UOPT_SPIN:
                                    step=(o->spin_max-o->spin_min)/20; if(step<1)step=1;
                                    if(e.button.button==3){
                                        /* right-click: quick coarse step down, no dialog */
                                        o->cur_spin-=step; if(o->cur_spin<o->spin_min)o->cur_spin=o->spin_min;
                                        snprintf(cmd,sizeof(cmd),"setoption name %s value %d",o->name,o->cur_spin);
                                        uci_send_raw(ei,cmd);
                                    } else {
                                        /* v14: left-click opens exact numeric entry — precise values like
                                           "Threads 2" or a Syzygy cache size can't be hit by coarse stepping */
                                        stropt_dialog_active=1; stropt_is_spin=1;
                                        stropt_dialog_opt_idx=oi;
                                        snprintf(stropt_dialog_buf,sizeof stropt_dialog_buf,"%d",o->cur_spin);
                                        stropt_dialog_len=strlen(stropt_dialog_buf);
                                        SDL_StartTextInput();
                                    }
                                    break;
                                case UOPT_COMBO:
                                    if(o->combo_count>0){
                                        if(e.button.button==3) o->combo_cur_idx=(o->combo_cur_idx-1+o->combo_count)%o->combo_count;
                                        else o->combo_cur_idx=(o->combo_cur_idx+1)%o->combo_count;
                                        snprintf(cmd,sizeof(cmd),"setoption name %s value %s",o->name,o->combo_vars[o->combo_cur_idx]);
                                        uci_send_raw(ei,cmd);
                                    }
                                    break;
                                case UOPT_STRING:
                                    stropt_dialog_active=1;
                                    stropt_dialog_opt_idx=oi;
                                    strncpy(stropt_dialog_buf,o->cur_str,255);
                                    stropt_dialog_len=strlen(stropt_dialog_buf);
                                    SDL_StartTextInput();
                                    break;
                                case UOPT_BUTTON:
                                    snprintf(cmd,sizeof(cmd),"setoption name %s",o->name);
                                    uci_send_raw(ei,cmd);
                                    sprintf(msg,"Sent: %s",o->name);
                                    break;
                            }
                        }
                    }
                    goto skip;
                }
                if(tourney_mgr_active){
                    int cmx=e.button.x, cmy=e.button.y;
                    int dw=TM_DW;
                    int dy=(WIN_H-TM_DH)/2; if(dy<10) dy=10; /* v17: same fixed anchor as draw_tourney_manager */
                    int dx=(WIN_W-dw)/2;
                    int dh = tourney_mgr_minimized ? 36 : TM_DH;
                    /* close X (always active, even minimized) */
                    if(cmx>=dx+dw-28&&cmx<=dx+dw-6&&cmy>=dy+6&&cmy<=dy+28){ tourney_mgr_active=0; tourney_mgr_minimized=0; goto skip; }
                    /* minimize / restore button */
                    if(cmx>=dx+dw-56&&cmx<=dx+dw-34&&cmy>=dy+6&&cmy<=dy+28){ tourney_mgr_minimized=!tourney_mgr_minimized; goto skip; }
                    if(tourney_mgr_minimized){
                        /* collapsed: clicking anywhere else on the title bar restores it;
                           clicking outside falls through so the board stays usable */
                        if(cmx>=dx&&cmx<=dx+dw&&cmy>=dy&&cmy<=dy+dh){ tourney_mgr_minimized=0; goto skip; }
                        goto tm_fallthrough;
                    }
                    if(cmx<dx||cmx>dx+dw||cmy<dy||cmy>dy+dh){ tourney_mgr_active=0; goto skip; }
                    int rx=dx+10, ry=dy+44, rw=360, rh=200;
                    int visible=rh/TM_ROW_H;
                    /* roster remove buttons */
                    if(cmx>=rx&&cmx<rx+rw&&cmy>=ry&&cmy<ry+rh){
                        int row=(cmy-ry)/TM_ROW_H;
                        int i=tm_roster_scroll+row;
                        int bx=rx+rw-26, by=ry+row*TM_ROW_H+2, bs=18;
                        if(i>=0&&i<roster_n&&cmx>=bx&&cmx<=bx+bs&&cmy>=by&&cmy<=by+bs){
                            if(tm_active) bottom_log_push("TM: stop the tournament before editing the roster");
                            else tm_roster_remove(i);
                        }
                        goto skip;
                    }
                    int aby=ry+rh+8;
                    if(cmy>=aby&&cmy<=aby+24){
                        if(cmx>=rx&&cmx<=rx+150){ /* + Add engine... */
                            if(!tm_active){
                                path_dialog_active=1; path_dialog_mode=1; path_dialog_buf[0]=0; path_dialog_len=0;
                                SDL_StartTextInput();
                            }
                            goto skip;
                        }
                        if(cmx>=rx+158&&cmx<=rx+158+140){ /* + Add Built-in */
                            if(!tm_active){
                                int idx=tm_roster_add_builtin();
                                if(idx>=0) snprintf(msg,sizeof msg,"Added to roster: %s",roster[idx].name);
                            }
                            goto skip;
                        }
                        if(cmx>=rx+306&&cmx<=rx+306+150){ /* + Add folder... (v16) */
                            if(!tm_active){
                                path_dialog_active=1; path_dialog_mode=2; path_dialog_buf[0]=0; path_dialog_len=0;
                                SDL_StartTextInput();
                            }
                            goto skip;
                        }
                    }
                    int sx=dx+dw-360, sy=dy+44;
                    if(cmx>=sx&&cmx<=sx+340&&cmy>=sy&&cmy<=sy+24){ tm_double_rr=!tm_double_rr; goto skip; }
                    if(cmy>=sy+30&&cmy<=sy+54){
                        if(cmx>=sx&&cmx<=sx+34){ if(tm_games_per_pairing>1) tm_games_per_pairing--; goto skip; }
                        if(cmx>=sx+38&&cmx<=sx+340){ if(tm_games_per_pairing<50) tm_games_per_pairing++; goto skip; }
                    }
                    if(cmy>=sy+60&&cmy<=sy+84){
                        if(cmx>=sx&&cmx<=sx+34){ tm_time_sec-=15; if(tm_time_sec<5) tm_time_sec=5; goto skip; }
                        if(cmx>=sx+38&&cmx<=sx+340){ tm_time_sec+=15; if(tm_time_sec>7200) tm_time_sec=7200; goto skip; }
                    }
                    if(cmy>=sy+90&&cmy<=sy+114){
                        if(cmx>=sx&&cmx<=sx+34){ tourney_delay_ms-=250; if(tourney_delay_ms<0) tourney_delay_ms=0; goto skip; }
                        if(cmx>=sx+38&&cmx<=sx+340){ tourney_delay_ms+=250; goto skip; }
                    }
                    if(cmx>=sx&&cmx<=sx+340&&cmy>=sy+126&&cmy<=sy+152){
                        if(tm_active) tm_stop(); else tm_start();
                        goto skip;
                    }
                    if(cmx>=sx&&cmx<=sx+166&&cmy>=sy+208&&cmy<=sy+230){ tm_export_standings(); goto skip; }
                    (void)visible;
                    goto skip;
                }
                tm_fallthrough:
                mx=e.button.x;my=e.button.y;
                if(e.button.button==3&&mx>=BOARD_OX&&mx<BOARD_OX+BRD&&my>=BOARD_OY&&my<BOARD_OY+BRD){
                    int row,col;px2sq(mx,my,&row,&col);
                    if(row>=0&&row<8&&col>=0&&col<8){arrow_dragging=1;arrow_fr=row;arrow_fc=col;arrow_mx=mx;arrow_my=my;}
                    goto skip;
                }
                if(my<MENU_H||open_menu>=0){handle_menu(mx,my);goto skip;}
                if(promo_pending){int ch=promo_click(mx,my);if(ch){Move pm;pm.fr=promo_fr;pm.fc=promo_fc;pm.tr=promo_tr;pm.tc=promo_tc;
                    int cp=bb_piece_at_rc(&B,promo_tr,promo_tc); /* v12 fix: credit capture on promotion too */
                    pm.cap=cp?abs(cp):0;
                    pm.promo=ch;pm.castle=0;pm.ep_cap=-1;apply(&pm);promo_pending=0;}goto skip;}
                if(!game_over&&(turn==player_color||both_human)&&(!ai_thinking||ai_is_ponder)&&mx>=BOARD_OX&&mx<BOARD_OX+BRD&&my>=BOARD_OY){
                    int row,col;px2sq(mx,my,&row,&col);if(!(row>=0&&row<8&&col>=0&&col<8))goto skip;
                    if(sel_r<0){
                        int p_at_sq=-1;
                        for(int t=0;t<6;t++) if(BB_GET(B.pieces[turn==WHITE?WC:BC][t],row*8+col)) p_at_sq=t+1;
                        if(p_at_sq!=-1){
                            sel_r=row;sel_c=col;
                            /* v12: begin drag */
                            drag_active=1;drag_r=row;drag_c=col;drag_piece=(turn==WHITE?1:-1)*p_at_sq;drag_mx=mx;drag_my=my;
                        }
                    }else{
                        int moved=attempt_move(sel_r,sel_c,row,col);
                        if(!moved){
                            sel_r=sel_c=-1;int p_at_sq=-1;
                            for(int t=0;t<6;t++) if(BB_GET(B.pieces[turn==WHITE?WC:BC][t],row*8+col)) p_at_sq=t+1;
                            if(p_at_sq!=-1){
                                sel_r=row;sel_c=col;
                                /* v12: begin drag on the newly selected piece */
                                drag_active=1;drag_r=row;drag_c=col;drag_piece=(turn==WHITE?1:-1)*p_at_sq;drag_mx=mx;drag_my=my;
                            }
                        }
                    }
                }
                skip:;
            }
            if(e.type==SDL_MOUSEBUTTONUP&&e.button.button!=3){
                /* v12: finish drag-and-drop */
                if(drag_active){
                    int row,col;px2sq(e.button.x,e.button.y,&row,&col);
                    int fr=drag_r,fc=drag_c;
                    drag_active=0;
                    if(row>=0&&row<8&&col>=0&&col<8&&!(row==fr&&col==fc)&&!game_over&&(turn==player_color||both_human)&&(!ai_thinking||ai_is_ponder)){
                        if(attempt_move(fr,fc,row,col)){ /* dropped on a legal square: move made, selection cleared by apply() path */ }
                    }
                }
            }
            if(e.type==SDL_MOUSEBUTTONUP&&e.button.button==3){
                if(arrow_dragging){
                    arrow_dragging=0;
                    int row,col;px2sq(e.button.x,e.button.y,&row,&col);
                    if(row>=0&&row<8&&col>=0&&col<8){
                        if(!(row==arrow_fr&&col==arrow_fc)){
                            if(arrow_count<10){arrows[arrow_count][0]=arrow_fr;arrows[arrow_count][1]=arrow_fc;arrows[arrow_count][2]=row;arrows[arrow_count][3]=col;arrow_count++;}
                        }
                    }
                }
            }
            /* v10: mouse wheel for options menu scroll */
            if(e.type==SDL_MOUSEWHEEL && (open_menu==4 || uci_opts_dialog_active)){
                if(e.wheel.y > 0) options_scroll = MAX(0, options_scroll - 1);
                else if(e.wheel.y < 0) options_scroll++;
            } else if(e.type==SDL_MOUSEWHEEL && tourney_mgr_active){
                if(e.wheel.y > 0) tm_roster_scroll = MAX(0, tm_roster_scroll - 1);
                else if(e.wheel.y < 0) tm_roster_scroll++;
            } else if(e.type==SDL_MOUSEWHEEL){
                int rw2=real_w>0?real_w:WIN_W, rh2=real_h>0?real_h:WIN_H;
                int y0=rh2-LOG_H;
                if(my>=y0 && my<rh2){
                    if(e.wheel.y>0) bottom_log_tab=(bottom_log_tab+2)%3;
                    else if(e.wheel.y<0) bottom_log_tab=(bottom_log_tab+1)%3;
                }
            }
        }
        SDL_SetRenderDrawColor(ren,0,0,0,255);SDL_RenderClear(ren);
        if(pgn_replay_auto && pgn_replay_mode && pgn_replay_index<pgn_replay_move_count){
            Uint32 now=SDL_GetTicks();
            if(now-pgn_replay_auto_last>=PGN_REPLAY_DELAY){
                pgn_replay_next();
                pgn_replay_auto_last=now;
                if(pgn_replay_index>=pgn_replay_move_count){
                    pgn_replay_auto=0;
                    sprintf(msg,"Replay finished");SDL_SetWindowTitle(win,msg);
                }
            }
        }
        if(pgn_replay_auto && pgn_replay_mode && pgn_replay_index>=pgn_replay_move_count){
            pgn_replay_auto=0;
            sprintf(msg,"Replay finished");SDL_SetWindowTitle(win,msg);
        }
        if(ponder_running&&(turn!=player_color||aivsai)){stop_pondering();}
        render(mx,my);
        if(!game_over&&!promo_pending){
            if(!aivsai&&turn!=player_color&&!ai_thinking&&!ai_done&&ponder_ready&&!pgn_replay_mode){
                if(hist_n>0){
                    Move *last_move=&hist[hist_n-1].m;
                    if(moves_equal(last_move,&ponder_opp_move)){
                        Move resp=ponder_resp_move;
                        Move leg[64];int lc;
                        Move all_moves[256];int n=bb_gen_moves(&B,turn==WHITE?WC:BC,all_moves);
                        lc=0;
                        for(int i=0;i<n;i++) if(all_moves[i].fr==resp.fr&&all_moves[i].fc==resp.fc&&all_moves[i].tr==resp.tr&&all_moves[i].tc==resp.tc){
                            BBoard bc;memcpy(&bc,&B,sizeof bc);bb_do(&bc,&all_moves[i],turn==WHITE?WC:BC);
                            if(!bb_inchk(&bc,turn==WHITE?WC:BC))leg[lc++]=all_moves[i];
                        }
                        if(lc){apply(&leg[0]);ponder_ready=0;stop_pondering();if(!game_over)start_pondering();continue;}
                    }
                }
                stop_pondering();
            }
            if((aivsai||turn!=player_color) && !pgn_replay_mode){
                /* v13 FIX: if both players are human (tourney W:Human B:Human),
                   don't start engine for either side */
                if(!both_human) {
                    if(!ai_thinking&&!ai_done)start_ai_move();
                    if(ai_done){ai_done=0;ai_is_ponder=0;apply_ai();}
                }
            }
        }
        /* v8.3.1: Analysis mode - show indicator, no ai_done apply */
        if(analysis_mode){ ai_done=0; }

        /* v9: Tournament auto-restart between games */
        if(tourney_waiting&&game_over){
            if(SDL_GetTicks()>=tourney_next_at){
                tourney_waiting=0;
                if(tm_active) tm_start_game(tm_sched_idx);
                else tourney_begin_game();
            }
        }

        SDL_Delay((aivsai && !anim_active && !drag_active) ? 100 : 16);
    }
    if(ai_thinking)stop_ai();
    if(ponder_running)stop_pondering();
    uci_close_engine(0); /* v10 */
    uci_close_engine(1); /* v10 */
    if(aud)SDL_CloseAudioDevice(aud);
    TTF_Quit();IMG_Quit();SDL_DestroyRenderer(ren);SDL_DestroyWindow(win);SDL_Quit();
    return 0;
}