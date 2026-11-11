// Linux window for the launcher (GTK 3). The logic is in core.c.
#include "core.h"
#include "ui.h"
#include <gtk/gtk.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>

static const char *const *T = UI_EN;
static GtkWidget *g_window, *g_folder, *g_log_view, *g_play, *g_uninstall;

static void log_line(const char *line) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_log_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, line, -1);
    gtk_text_buffer_insert(buf, &end, "\n", 1);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(g_log_view), gtk_text_buffer_get_insert(buf), 0, FALSE, 0, 0);
    while (gtk_events_pending()) gtk_main_iteration();  // keep the window alive during long steps
}

static const char *game_dir(void) { return gtk_entry_get_text(GTK_ENTRY(g_folder)); }

static void on_browse(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    GtkWidget *d = gtk_file_chooser_dialog_new(T[T_PICK], GTK_WINDOW(g_window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        gtk_entry_set_text(GTK_ENTRY(g_folder), dir);
        log_line(core_check_game(dir) ? T[T_FOUND] : T[T_BAD_FOLDER]);
        g_free(dir);
    }
    gtk_widget_destroy(d);
}

static void set_busy(gboolean busy) {
    gtk_widget_set_sensitive(g_play, !busy);
    gtk_widget_set_sensitive(g_uninstall, !busy);
}

static void on_play(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    char save[1024];
    if (!core_check_game(game_dir())) { log_line(T[T_BAD_FOLDER]); return; }
    set_busy(TRUE);
    if (core_find_save_dir(game_dir(), save, sizeof save)) core_backup_save(save);
    else log_line(T[T_NO_SAVE]);
    if (core_build(game_dir())) core_launch();
    set_busy(FALSE);
}

static void on_uninstall(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, "%s", T[T_CONFIRM_UNINSTALL]);
    int answer = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (answer != GTK_RESPONSE_OK) return;
    char save[1024] = "";
    core_find_save_dir(game_dir(), save, sizeof save);
    core_uninstall(save);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    const char *lang = g_getenv("LC_ALL") ? g_getenv("LC_ALL") : g_getenv("LC_MESSAGES") ? g_getenv("LC_MESSAGES") : g_getenv("LANG");
    if (lang && !strncmp(lang, "pl", 2)) T = UI_PL;

    char exe[4096] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) exe[n] = 0;
    core_set_app_dir(n > 0 ? dirname(exe) : ".");
    core_set_log(log_line);

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), T[T_TITLE]);
    gtk_window_set_default_size(GTK_WINDOW(g_window), 640, 420);
    gtk_container_set_border_width(GTK_CONTAINER(g_window), 12);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(g_window), box);
    GtkWidget *label = gtk_label_new(T[T_GAME_FOLDER]);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_folder = gtk_entry_new();
    GtkWidget *browse = gtk_button_new_with_label(T[T_BROWSE]);
    gtk_box_pack_start(GTK_BOX(row), g_folder, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    g_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_log_view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), g_log_view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_play = gtk_button_new_with_label(T[T_PLAY]);
    g_uninstall = gtk_button_new_with_label(T[T_UNINSTALL]);
    gtk_box_pack_start(GTK_BOX(buttons), g_play, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), g_uninstall, FALSE, FALSE, 0);
    GtkWidget *close = gtk_button_new_with_label(T[T_CLOSE]);
    gtk_box_pack_start(GTK_BOX(buttons), close, FALSE, FALSE, 0);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), g_window);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), NULL);
    g_signal_connect(g_play, "clicked", G_CALLBACK(on_play), NULL);
    g_signal_connect(g_uninstall, "clicked", G_CALLBACK(on_uninstall), NULL);

    gtk_widget_show_all(g_window);
    char game[1024];
    if (core_find_game(game, sizeof game)) {
        gtk_entry_set_text(GTK_ENTRY(g_folder), game);
        log_line(T[T_FOUND]);
    } else {
        log_line(T[T_NOT_FOUND]);
    }
    gtk_main();
    return 0;
}
