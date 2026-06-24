/*
 * Copyright (c) 2026 luke8086
 * Distributed under the terms of GPL-2 License
 *
 * File: klondike.c - Klondike solitaire
 */

#include <gui.h>

enum {
    CARD_WIDTH = 38,
    CARD_HEIGHT = 44,

    COLUMN_CARDS_STEP = 11,
    COLUMN_CARDS_MAX = 24,

    CARD_COUNT = 52,
    FOUND_COUNT = 4,
    COLUMN_COUNT = 7,
    PILE_COUNT = 1 + 1 + FOUND_COUNT + COLUMN_COUNT,

    PAD_X = 8,
    PAD_Y = 8,
    GAP_X = 4,
    GAP_Y = 8,
    HOLDS_Y = TITLE_BAR_HEIGHT + PAD_Y,
    COLUMNS_Y = HOLDS_Y + CARD_HEIGHT + GAP_Y,
    COLUMNS_H = 150,

    WINDOW_WIDTH = 2 * PAD_X + COLUMN_COUNT * CARD_WIDTH + (COLUMN_COUNT - 1) * GAP_X,
    WINDOW_HEIGHT = COLUMNS_Y + COLUMNS_H + PAD_Y,

    PILE_STOCK = 1,
    PILE_WASTE = 2,
    PILE_FOUNDS = 3,
    PILE_COLUMNS = 4,

    PILE_STOCK_IDX = 0,
    PILE_WASTE_IDX = 1,
    PILE_FOUNDS_IDX = 2,
    PILE_COLUMNS_IDX = PILE_FOUNDS_IDX + FOUND_COUNT,

    STATE_DEFAULT = 0,
    STATE_AUTO_PENDING = 1,
    STATE_WON = 2,

    AUTO_MOVE_HIGHLIGHT_TICKS = 2,
    AUTO_MOVE_EXECUTE_TICKS = 6,
};

static surface_st window_surface;
static window_st window;

static widget_st title_bar;
static widget_st close_button;
static widget_st pile_widgets[PILE_COUNT];
static widget_st *widgets[PILE_COUNT + 2];

static card_t stock_cards[CARD_COUNT];
static card_pile_st stock;

static card_t waste_cards[CARD_COUNT];
static card_pile_st waste;

static card_t found_cards[FOUND_COUNT][1];
static card_pile_st founds[FOUND_COUNT];

static card_t column_cards[COLUMN_COUNT][COLUMN_CARDS_MAX];
static card_pile_st columns[COLUMN_COUNT];

static card_pile_st *all_piles[PILE_COUNT];
static card_game_st game;
static int state;
static int ticks_waited;

static card_pile_st *press_pile;
static point_st press_origin;
static int dragged;
static int press_grabbed;

static void
draw_all_piles(void)
{
    for (int i = 0; i < PILE_COUNT; ++i) {
        card_pile_draw(&game, all_piles[i]);
    }
}

static void
draw_window(window_st * window)
{
    gui_window_draw(window, COLOR_WIDGET_BG);
    draw_all_piles();
}

static int
remaining_cards(void)
{
    int i, ret;

    ret = stock.count + waste.count;

    for (i = 0; i < COLUMN_COUNT; ++i) {
        ret += columns[i].count;
    }

    return ret;
}

static void
deal_cards(void)
{
    card_t deck[CARD_COUNT];
    int i, j, k;

    card_deck_init(deck, CARD_COUNT);
    card_deck_shuffle(deck, CARD_COUNT);

    stock.count = 0;
    waste.count = 0;

    for (i = 0; i < FOUND_COUNT; ++i) {
        founds[i].count = 0;
    }

    for (i = 0; i < COLUMN_COUNT; ++i) {
        columns[i].count = 0;
        columns[i].face_up_from = 0;
    }

    k = 0;
    for (i = 0; i < COLUMN_COUNT; ++i) {
        for (j = 0; j <= i; ++j) {
            card_pile_push(&columns[i], deck[k++]);
        }
        columns[i].face_up_from = i;
    }

    while (k < CARD_COUNT) {
        card_pile_push(&stock, deck[k++]);
    }

    game.cur_move.src = NULL;
    state = STATE_DEFAULT;
}

static void
update_status(void)
{
    if (state == STATE_WON) {
        gui_status_set("You won! Press R to restart");
        return;
    }

    gui_status_set("Remaining: %d  \xb3  R: Restart", remaining_cards());
}

static void
check_win(void)
{
    int i;

    for (i = 0; i < FOUND_COUNT; ++i) {
        if (founds[i].count == 0 || CARD_RANK(CARD_PILE_TOP(&founds[i])) != 12) {
            return;
        }
    }

    state = STATE_WON;
    update_status();
}

static int
get_max_valid_sequence_len(card_pile_st *p)
{
    int i;
    card_t curr, prev;

    if (p->count == 0) {
        return 0;
    }

    for (i = p->count - 1; i > p->face_up_from; --i) {
        curr = p->cards[i];
        prev = p->cards[i - 1];

        if (CARD_RANK(prev) != CARD_RANK(curr) + 1) {
            break;
        }
        if (CARD_COLOR(prev) == CARD_COLOR(curr)) {
            break;
        }
    }

    return p->count - i;
}

static int
card_should_auto_promote(card_t card)
{
    int rank = CARD_RANK(card);
    int suit = CARD_SUIT(card);
    int color = CARD_COLOR(card);
    int i;

    if (founds[suit].count == 0) {
        if (rank != 0) {
            return 0;
        }
    } else if (rank != CARD_RANK(CARD_PILE_TOP(&founds[suit])) + 1) {
        return 0;
    }

    if (rank <= 1) {
        return 1;
    }

    for (i = 0; i < FOUND_COUNT; ++i) {
        if (CARD_COLOR(i * 13) == color) {
            continue;
        }

        if (founds[i].count == 0 || CARD_RANK(CARD_PILE_TOP(&founds[i])) < rank - 1) {
            return 0;
        }
    }

    return 1;
}

static void
set_auto_move(card_pile_st *src, card_pile_st *dst)
{
    game.cur_move.src = src;
    game.cur_move.dst = dst;
    game.cur_move.count = 1;
    state = STATE_AUTO_PENDING;
    ticks_waited = 0;
}

static void
check_auto_move(void)
{
    int i;
    card_t card;

    card = CARD_PILE_TOP(&waste);

    if (card != CARD_EMPTY && card_should_auto_promote(card)) {
        set_auto_move(&waste, &founds[CARD_SUIT(card)]);
        return;
    }

    for (i = 0; i < COLUMN_COUNT; ++i) {
        card = CARD_PILE_TOP(&columns[i]);

        if (card != CARD_EMPTY && card_should_auto_promote(card)) {
            set_auto_move(&columns[i], &founds[CARD_SUIT(card)]);
            return;
        }
    }
}

static void
exec_move(void)
{
    card_game_exec_cur_move(&game);
    update_status();
    check_win();

    if (state != STATE_WON) {
        check_auto_move();
    }
}

static void
start_move(card_pile_st *pile, int count)
{
    game.cur_move.src = pile;
    game.cur_move.count = count;
    card_pile_draw(&game, pile);
    update_status();
}

static void
cancel_move(void)
{
    card_pile_st *old = game.cur_move.src;

    game.cur_move.src = NULL;

    if (old) {
        card_pile_draw(&game, old);
    }

    update_status();
}

static void
show_error(const char *msg)
{
    cancel_move();
    gui_status_set("%s", msg);
}

static int
can_move_to_column(card_pile_st *col, card_t src_bottom)
{
    card_t dst_top;

    if (col->count == 0) {
        return CARD_RANK(src_bottom) == 12;
    }

    dst_top = CARD_PILE_TOP(col);

    if (CARD_RANK(dst_top) != CARD_RANK(src_bottom) + 1) {
        return 0;
    }

    if (CARD_COLOR(dst_top) == CARD_COLOR(src_bottom)) {
        return 0;
    }

    return 1;
}

static void
request_move_to_column(void)
{
    card_pile_st *src = game.cur_move.src;
    card_pile_st *dst = game.cur_move.dst;
    int n = game.cur_move.count;
    card_t src_bottom;

    if (src->type == PILE_COLUMNS && n > get_max_valid_sequence_len(src)) {
        show_error("Invalid move");
        return;
    }

    src_bottom = src->cards[src->count - n];

    if (!can_move_to_column(dst, src_bottom)) {
        show_error("Invalid move");
        return;
    }

    exec_move();
}

static int
can_move_to_found(card_pile_st *found, card_t card)
{
    if (CARD_SUIT(card) != found->index) {
        return 0;
    }

    if (found->count == 0) {
        return CARD_RANK(card) == 0;
    }

    return CARD_RANK(CARD_PILE_TOP(found)) + 1 == CARD_RANK(card);
}

static void
request_move_to_found(void)
{
    card_pile_st *src = game.cur_move.src;
    card_pile_st *found = game.cur_move.dst;

    if (game.cur_move.count != 1) {
        show_error("Invalid move");
        return;
    }

    card_t card = CARD_PILE_TOP(src);

    if (!can_move_to_found(found, card)) {
        show_error("Invalid move");
        return;
    }

    exec_move();
}

static void
request_move(void)
{
    card_pile_st *dst = game.cur_move.dst;

    if (dst->type == PILE_COLUMNS) {
        request_move_to_column();
    } else if (dst->type == PILE_FOUNDS) {
        request_move_to_found();
    } else {
        show_error("Invalid move");
    }
}

static void
draw_card_from_stock(void)
{
    if (stock.count > 0) {
        card_pile_push(&waste, card_pile_pop(&stock));
    } else if (waste.count > 0) {
        while (waste.count > 0) {
            card_pile_push(&stock, card_pile_pop(&waste));
        }
    }

    card_pile_draw(&game, &stock);
    card_pile_draw(&game, &waste);
    update_status();
    check_auto_move();
}

static void
restart_game(void)
{
    deal_cards();
    draw_all_piles();
    update_status();
}

static void
pile_draw(widget_st *widget)
{
    card_pile_draw(&game, all_piles[widget->tag1]);
}

static card_pile_st *
pile_under(point_st pos)
{
    widget_st *w = gui_window_find_widget_at(&window, pos);

    if (w >= pile_widgets && w < pile_widgets + PILE_COUNT) {
        return all_piles[w - pile_widgets];
    }

    return NULL;
}

static void
on_pile_pointer_down(widget_st *widget, event_st event _unsd, point_st pos)
{
    if (state != STATE_DEFAULT) {
        return;
    }

    card_pile_st *pile = all_piles[widget->tag1];

    press_pile = pile;
    press_origin = pos;
    dragged = 0;
    press_grabbed = 0;

    if (pile->type == PILE_STOCK) {
        return;
    }

    if (game.cur_move.src == NULL) {
        if (pile->type == PILE_FOUNDS || pile->count == 0) {
            return;
        }

        int idx = card_pile_get_card_index_by_ypos(pile, pos.y - widget->rect.y);

        if (pile->type == PILE_COLUMNS && idx < pile->face_up_from) {
            return;
        }

        start_move(pile, pile->count - idx);
        press_grabbed = 1;
    }
}

static void
on_pile_pointer_move(widget_st *widget _unsd, event_st event _unsd, point_st pos)
{
    int dx = pos.x - press_origin.x;
    int dy = pos.y - press_origin.y;

    if (!press_pile) {
        return;
    }

    if (dx > 3 || dx < -3 || dy > 3 || dy < -3) {
        dragged = 1;
    }
}

static void
on_pile_pointer_up(widget_st *widget _unsd, event_st event _unsd, point_st pos)
{
    if (state != STATE_DEFAULT) {
        return;
    }

    card_pile_st *pressed = press_pile;
    card_pile_st *release_pile = pile_under(pos);

    press_pile = NULL;

    if (pressed && pressed->type == PILE_STOCK && !dragged) {
        cancel_move();
        draw_card_from_stock();
        return;
    }

    if (game.cur_move.src == NULL) {
        return;
    }

    if (release_pile && release_pile != game.cur_move.src) {
        game.cur_move.dst = release_pile;
        request_move();
        return;
    }

    if (dragged) {
        cancel_move();
    } else if (!press_grabbed) {
        cancel_move();
    }
}

static void
on_pile_pointer_alt(widget_st *widget, event_st event _unsd, point_st pos _unsd)
{
    if (state != STATE_DEFAULT) {
        return;
    }

    card_pile_st *pile = all_piles[widget->tag1];

    if (pile->type != PILE_WASTE && pile->type != PILE_COLUMNS) {
        return;
    }

    if (pile->count == 0) {
        return;
    }

    cancel_move();
    start_move(pile, 1);

    card_t card = CARD_PILE_TOP(pile);
    game.cur_move.dst = &founds[CARD_SUIT(card)];
    request_move();
}

static void
on_key_down(window_st *win _unsd, event_st event)
{
    if (state == STATE_AUTO_PENDING) {
        return;
    }

    if (event.key_code == KEY_R) {
        restart_game();
    }
}

static void
on_tick(window_st *win _unsd)
{
    if (state != STATE_AUTO_PENDING) {
        return;
    }

    ++ticks_waited;

    if (ticks_waited == AUTO_MOVE_HIGHLIGHT_TICKS) {
        card_pile_draw(&game, game.cur_move.src);
        return;
    }

    if (ticks_waited >= AUTO_MOVE_EXECUTE_TICKS) {
        state = STATE_DEFAULT;
        exec_move();
    }
}

static void
on_active_change(window_st *w)
{
    if (w->active) {
        update_status();
    }
}

static void
init_game(void)
{
    int i;
    int x0 = PAD_X;
    int step_x = CARD_WIDTH + GAP_X;

    game.surface = &window_surface;
    game.card_width = CARD_WIDTH;
    game.card_height = CARD_HEIGHT;
    game.card_step = COLUMN_CARDS_STEP;

    for (i = 0; i < PILE_COUNT; ++i) {
        pile_widgets[i].draw = pile_draw;
        pile_widgets[i].on_pointer_down = on_pile_pointer_down;
        pile_widgets[i].on_pointer_move = on_pile_pointer_move;
        pile_widgets[i].on_pointer_up = on_pile_pointer_up;
        pile_widgets[i].on_pointer_alt = on_pile_pointer_alt;
        pile_widgets[i].press_sticky = 1;
        pile_widgets[i].tag1 = i;
    }

    stock.type = PILE_STOCK;
    stock.capacity = CARD_COUNT;
    stock.cards = stock_cards;
    stock.face_up_from = CARD_PILE_ALL_FACE_DOWN;
    stock.widget = &pile_widgets[PILE_STOCK_IDX];
    stock.widget->rect = gui_rect_make(x0 + 0 * step_x, HOLDS_Y, CARD_WIDTH, CARD_HEIGHT);
    all_piles[PILE_STOCK_IDX] = &stock;

    waste.type = PILE_WASTE;
    waste.capacity = CARD_COUNT;
    waste.cards = waste_cards;
    waste.widget = &pile_widgets[PILE_WASTE_IDX];
    waste.widget->rect = gui_rect_make(x0 + 1 * step_x, HOLDS_Y, CARD_WIDTH, CARD_HEIGHT);
    all_piles[PILE_WASTE_IDX] = &waste;

    for (i = 0; i < FOUND_COUNT; ++i) {
        founds[i].type = PILE_FOUNDS;
        founds[i].index = i;
        founds[i].capacity = 1;
        founds[i].cards = found_cards[i];
        founds[i].replace_on_push = 1;
        founds[i].widget = &pile_widgets[PILE_FOUNDS_IDX + i];
        founds[i].widget->rect = gui_rect_make(x0 + (i + 3) * step_x, HOLDS_Y,
            CARD_WIDTH, CARD_HEIGHT);
        all_piles[PILE_FOUNDS_IDX + i] = &founds[i];
    }

    for (i = 0; i < COLUMN_COUNT; ++i) {
        columns[i].type = PILE_COLUMNS;
        columns[i].index = i;
        columns[i].capacity = COLUMN_CARDS_MAX;
        columns[i].cards = column_cards[i];
        columns[i].is_cascade = 1;
        columns[i].widget = &pile_widgets[PILE_COLUMNS_IDX + i];
        columns[i].widget->rect = gui_rect_make(x0 + i * step_x, COLUMNS_Y,
            CARD_WIDTH, COLUMNS_H);
        all_piles[PILE_COLUMNS_IDX + i] = &columns[i];
    }

    for (i = 0; i < PILE_COUNT; ++i) {
        gui_window_add_widget(&window, &pile_widgets[i]);
    }
}

static void
init_window(void)
{
    window_surface.size.width = WINDOW_WIDTH;
    window_surface.size.height = WINDOW_HEIGHT;
    window_surface.pitch = WINDOW_WIDTH;
    window_surface.pixels = krn_heap_alloc(WINDOW_WIDTH * WINDOW_HEIGHT, "Klondike pixels", 1);

    window.surface = &window_surface;
    window.title = "Klondike";
    window.widgets = widgets;
    window.widgets_capacity = sizeof(widgets) / sizeof(widgets[0]);
    window.draw = draw_window;
    window.on_active_change = on_active_change;
    window.on_key_down = on_key_down;
    window.on_tick = on_tick;

    gui_window_init_frame(&window, &title_bar, &close_button);
}

static void
init_app(void)
{
    init_window();
    init_game();

    restart_game();
}

static void
show_app(void)
{
    (void)gui_wm_add_window(&window);
}

global app_st app_klondike = {
    .icon = &icon_klondike,
    .init = init_app,
    .show = show_app,
};
