"""
make_book.py — конвертира Elo2400.bin -> book.h
Инсталирай: pip install chess --break-system-packages
Пусни: python make_book.py
"""
import sys, os

try:
    import chess
    import chess.polyglot
except ImportError:
    print("Инсталирай: pip install chess --break-system-packages")
    sys.exit(1)

BOOK_FILE = r"C:\Chess\book\Elo2400.bin"
OUT_FILE  = r"C:\Chess\book.h"

# ---- Same LCG as chess.c (seed=42) ----
def make_randoms():
    state = 42
    R = []
    for _ in range(781):
        state = (state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        R.append(state)
    return R

R = make_randoms()

def my_hash(board):
    h = 0
    for py_sq in range(64):
        p = board.piece_at(py_sq)
        if not p:
            continue
        rank, file = py_sq // 8, py_sq % 8
        my_sq = (7 - rank) * 8 + file
        color = 1 if p.color == chess.WHITE else 0
        pidx  = (p.piece_type - 1) * 2 + color  # BP=0,WP=1,BN=2,WN=3,...
        h ^= R[pidx * 64 + my_sq]
    if board.has_kingside_castling_rights(chess.WHITE):  h ^= R[768]
    if board.has_queenside_castling_rights(chess.WHITE): h ^= R[769]
    if board.has_kingside_castling_rights(chess.BLACK):  h ^= R[770]
    if board.has_queenside_castling_rights(chess.BLACK): h ^= R[771]
    if board.ep_square is not None:
        h ^= R[772 + (board.ep_square % 8)]
    if board.turn == chess.WHITE:
        h ^= R[780]
    return h

def move_to_my(m):
    """Convert python-chess square to my (row,col)"""
    def conv(sq):
        return (7 - sq // 8), (sq % 8)
    fr, fc = conv(m.from_square)
    tr, tc = conv(m.to_square)
    promo  = m.promotion if m.promotion else 0
    # python-chess promo: QUEEN=5,ROOK=4,BISHOP=3,KNIGHT=2
    # my engine uses same values (QUEEN=5 etc)
    return fr, fc, tr, tc, promo

if not os.path.exists(BOOK_FILE):
    print(f"Файлът не е намерен: {BOOK_FILE}")
    sys.exit(1)

print("Четене на книга...")
entries = {}  # key -> (fr,fc,tr,tc,promo,weight)

with chess.polyglot.open_reader(BOOK_FILE) as reader:
    queue  = [chess.Board()]
    seen   = set()
    count  = 0

    while queue:
        board = queue.pop(0)
        fen   = board.fen()
        if fen in seen:
            continue
        seen.add(fen)

        book_moves = list(reader.find_all(board))
        if not book_moves:
            continue

        h = my_hash(board)
        for e in book_moves:
            fr,fc,tr,tc,pr = move_to_my(e.move)
            key = (h, fr, fc, tr, tc, pr)
            if key not in entries or e.weight > entries[key]:
                entries[key] = e.weight
            count += 1

        # BFS deeper (limit depth to keep size reasonable)
        if board.fullmove_number <= 12:
            for e in book_moves:
                nb = board.copy()
                nb.push(e.move)
                queue.append(nb)

print(f"Намерени {len(entries)} уникални позиции/хода")

# Sort by hash for binary search in C
sorted_entries = sorted(entries.items())  # ((h,fr,fc,tr,tc,pr), weight)

print(f"Запис на {OUT_FILE} ...")
with open(OUT_FILE, "w") as f:
    f.write("/* Auto-generated from Elo2400.bin — не редактирай ръчно */\n")
    f.write("#ifndef BOOK_H\n#define BOOK_H\n")
    f.write("#include <stdint.h>\n\n")
    f.write("typedef struct { uint64_t key; uint8_t fr,fc,tr,tc,promo; uint16_t w; } BK;\n\n")
    f.write(f"#define BOOK_SIZE {len(sorted_entries)}\n\n")
    f.write("static const BK BOOK_DATA[] = {\n")
    for (h,fr,fc,tr,tc,pr), w in sorted_entries:
        f.write(f"  {{0x{h:016X}ULL,{fr},{fc},{tr},{tc},{pr},{w}}},\n")
    f.write("};\n\n#endif\n")

print("Готово! Сега компилирай chess.c")
