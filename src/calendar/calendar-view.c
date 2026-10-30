/* calendar-view.c – Kalender-Ansicht (Prototyp #339, #373)
 *
 * Vorlage: mail/mail-view.c (Template-Klasse mit Eigenschaft „stack“).
 * Oberfläche in calendar-view.blp. #373: Kalender und Termine über die aus GNOME Calendar 50.0
 * übernommene Schicht in src/calendar/gcal (GcalContext → GcalManager → GcalTimeline, je Kalender ein
 * GcalCalendarMonitor mit ECalClientView); Monat = GcalMonthView. Die Anbindung folgt GcalWindow
 * (gcal-window.c). #374: Woche = GcalWeekView, Tag = GcalWeekView mit 1 Spalte (n-days); alle drei Ansichten
 * sind Abonnenten derselben Timeline, das gemeinsame Datum ist der Tag der Monatsauswahl in der Seitenleiste.
 * Aktionen ohne Funktion sind nicht registriert, ihre Knöpfe sind ausgegraut.
 * #375: Termine anlegen, ansehen, bearbeiten, löschen wie GcalWindow (gcal-window.c:601-627 new-event, 869-946
 * create_event_detailed_cb/event_preview_cb/event_activated, 947-1001 Löschen mit Rückgängig-Toast) mit
 * GcalEventPopover und GcalEventEditorDialog; Ablauf des Löschens wie Kontakte #370 (contacts-view.c execute_pending:
 * nach Ablauf, beim nächsten Löschen und beim Schließen des Fensters sofort). Fehler als AdwBanner (Architektur 3).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "calendar-view.h"
#include "gcal/gcal-context.h"
#include "gcal/gcal-date-time-utils.h"
#include "gcal/gcal-event-editor-dialog.h"
#include "gcal/gcal-event-widget.h"
#include "gcal/gcal-month-view.h"
#include "gcal/gcal-timeline.h"
#include "gcal/gcal-timeline-subscriber.h"
#include "gcal/gcal-utils.h"
#include "gcal/gcal-view.h"
#include "gcal/gcal-week-view.h"

/* #375: vorgemerktes Löschen (Rückgängig-Toast). */
typedef struct
{
  GcalEvent             *event;
  GcalRecurrenceModType  mod;
  AdwToast              *toast;
  guint                  timeout_id;
  GWeakRef               view;          /* beim Schreiben: die Ansicht, falls sie noch lebt */
} PendingDelete;

struct _RevolutionCalendarView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  AdwOverlaySplitView *split;
  AdwViewStack        *views;
  GtkSortListModel    *calendar_sorted;
  GtkWidget           *month_view;
  GtkWidget           *week_view;
  GtkWidget           *day_view;
  GtkCalendar         *date_chooser;
  GtkLabel            *month_title;
  GtkCssProvider      *colors;
  GtkStack            *header_stack;
  GtkWidget           *app_switcher;
  GtkSearchEntry      *search_entry;
  GtkToggleButton     *search_button;
  GcalContext         *context;
  ESourceRegistry     *registry;
  gboolean             started;
  AdwBanner           *banner;          /* #375 */
  GcalEventEditorDialog *editor;
  GcalEventWidget     *preview;         /* Termin mit offenem Popover (schwach) */
  PendingDelete       *pending;
};

G_DEFINE_FINAL_TYPE (RevolutionCalendarView, revolution_calendar_view, ADW_TYPE_BREAKPOINT_BIN)

/* calendar.toggle-sidebar fürs Hauptmenü (#402) wie mail.toggle-sidebar
 * (mail-view.c on_toggle_sidebar): wie calendar.sidebar (F9), aber zustandslos,
 * damit GtkPopoverMenu keine Haken-Spalte einrückt. */
static void
on_toggle_sidebar (GSimpleAction *action G_GNUC_UNUSED,
                   GVariant      *parameter G_GNUC_UNUSED,
                   gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;

  adw_overlay_split_view_set_show_sidebar (self->split, !adw_overlay_split_view_get_show_sidebar (self->split));
}

/* #402 Runde 2: Suche übernommen aus mail-view.c (focus_search_entry, on_search_toggled,
 * on_search_started, on_search_stop, on_search). Gefiltert wird noch nichts. */

/* Fokus ins Suchfeld ohne alles zu markieren: sonst ersetzt beim Tippen im
 * Hauptbereich (key-capture-widget) der zweite Buchstabe den ersten. */
static void
focus_search_entry (RevolutionCalendarView *self)
{
  GtkEditable *text = gtk_editable_get_delegate (GTK_EDITABLE (self->search_entry));

  gtk_text_grab_focus_without_selecting (GTK_TEXT (text));
}

/* Suchknopf schaltet die Kopfleiste zwischen Titel und Suchfeld um („Lights off“). */
static void
on_search_toggled (RevolutionCalendarView *self)
{
  gboolean active = gtk_toggle_button_get_active (self->search_button);

  /* #454: Das Suchfeld braucht die ganze Breite, der Umschalter steht sonst mittig im Weg. */
  gtk_widget_set_visible (self->app_switcher, !active);

  if (active)
    {
      gtk_stack_set_visible_child_name (self->header_stack, "search");
      focus_search_entry (self);
    }
  else
    {
      gtk_stack_set_visible_child_name (self->header_stack, "actions");
      gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
      gtk_widget_grab_focus (GTK_WIDGET (self->search_button));
    }
}

/* Tippen im Hauptbereich (GtkSearchEntry::search-started) öffnet die Suche. */
static void
on_search_started (RevolutionCalendarView *self)
{
  gtk_toggle_button_set_active (self->search_button, TRUE);
}

/* Escape im Suchfeld (GtkSearchEntry::stop-search). */
static void
on_search_stop (RevolutionCalendarView *self)
{
  gtk_toggle_button_set_active (self->search_button, FALSE);
}

/* calendar.search für Strg+F: öffnet nur, schließt nicht (Schließen: Escape, Suchknopf).
 * Ist die Suche schon offen, holt es den Fokus zurück ins Suchfeld. */
static void
on_search (GSimpleAction *action G_GNUC_UNUSED,
           GVariant      *parameter G_GNUC_UNUSED,
           gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;

  if (gtk_toggle_button_get_active (self->search_button))
    focus_search_entry (self);
  else
    gtk_toggle_button_set_active (self->search_button, TRUE);
}

static void on_previous (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_next (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_today (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_switch_view (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_new_event (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_delete_event (GSimpleAction *action, GVariant *parameter, gpointer user_data);

static const GActionEntry calendar_actions[] = {
  { "previous", on_previous, NULL, NULL, NULL, { 0 } },
  { "next", on_next, NULL, NULL, NULL, { 0 } },
  { "today", on_today, NULL, NULL, NULL, { 0 } },
  { "switch-view", on_switch_view, "i", NULL, NULL, { 0 } },
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  { "toggle-sidebar", on_toggle_sidebar, NULL, NULL, NULL, { 0 } },
  { "new-event", on_new_event, "s", NULL, NULL, { 0 } },
  { "delete-event", on_delete_event, NULL, NULL, NULL, { 0 } },
};

/* #402: Klasse .dark für Regeln nur im dunklen Design (style.css), wie mail-view.c on_dark_changed. */
static void
on_dark_changed (AdwStyleManager        *manager,
                 GParamSpec             *pspec G_GNUC_UNUSED,
                 RevolutionCalendarView *self)
{
  if (adw_style_manager_get_dark (manager))
    gtk_widget_add_css_class (GTK_WIDGET (self), "dark");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "dark");
}

enum { PROP_0, PROP_STACK, N_PROPS };

/* Kalenderfarben (#373): je Farbe eine Klasse .color-<Quark> mit --event-bg-color für die Termine
 * (GcalEventWidget, events.css), übernommen aus gnome-calendar 50.0 gcal-window.c:345-372
 * (recalculate_calendar_colors_css). Dazu das Ankreuzfeld des Kalenders in der Seitenleiste
 * (Widget-Name = Klasse, calendar_color_name), wie im Prototyp #339/#402. Farbe nur gelesen. */
static void
recalculate_calendar_colors_css (RevolutionCalendarView *self)
{
  g_autoptr (GString) css_colors = NULL;
  g_autoptr (GList) calendars = NULL;
  GcalManager *manager;

  css_colors = g_string_new (NULL);
  manager = gcal_context_get_manager (self->context);
  calendars = gcal_manager_get_calendars (manager);
  for (GList *l = calendars; l; l = l->next)
    {
      g_autofree gchar* color_str = NULL;
      const GdkRGBA *color;
      GcalCalendar *calendar;
      GQuark color_id;

      calendar = GCAL_CALENDAR (l->data);

      color = gcal_calendar_get_color (calendar);
      color_str = gdk_rgba_to_string (color);
      color_id = g_quark_from_string (color_str);

      g_string_append_printf (css_colors, ".color-%u { --event-bg-color: %s; }\n", color_id, color_str);
      g_string_append_printf (css_colors,
                              "checkbutton#color-%1$u > check { box-shadow: inset 0 0 0 2px %2$s; }\n"
                              "checkbutton#color-%1$u > check:checked { background-color: %2$s; color: var(--accent-fg-color); box-shadow: none; }\n",
                              color_id, color_str);
    }

  gtk_css_provider_load_from_string (self->colors, css_colors->str);
}

/* Log je verbundenem Kalender für tests/kalender-treiber.py (Vergleich mit tests/testdaten.py --kalender). */
static void
on_calendar_added (RevolutionCalendarView *self,
                   GcalCalendar           *calendar)
{
  ESource *parent = gcal_calendar_get_parent_source (calendar);
  ESourceSelectable *extension = e_source_get_extension (gcal_calendar_get_source (calendar), E_SOURCE_EXTENSION_CALENDAR);

  g_debug ("Kalender: %s %s / %s farbe %s nurlesend %d sichtbar %d", gcal_calendar_get_id (calendar),
           parent != NULL ? e_source_get_display_name (parent) : "-", gcal_calendar_get_name (calendar),
           e_source_selectable_get_color (extension), gcal_calendar_is_read_only (calendar),
           gcal_calendar_get_visible (calendar));
  recalculate_calendar_colors_css (self);
}

/* Widget-Name des Ankreuzfelds = Farbklasse wie GcalEventWidget update_color (gcal-event-widget.c). */
static char *
calendar_color_name (GtkListItem   *item G_GNUC_UNUSED,
                     const GdkRGBA *color)
{
  g_autofree char *color_str = color != NULL ? gdk_rgba_to_string (color) : NULL;

  return color_str != NULL ? g_strdup_printf ("color-%u", g_quark_from_string (color_str)) : NULL;
}

/* Abschnitt je Konto (Eltern-Source), „Auf diesem Rechner“ zuerst wie contacts-book.c „section“. */
static char *
calendar_section (GcalCalendar *calendar G_GNUC_UNUSED,
                  ESource      *parent)
{
  if (parent == NULL)
    return g_strdup ("2");

  return g_strdup_printf ("%d%s/%s", g_strcmp0 (e_source_get_uid (parent), "local-stub") == 0 ? 0 : 1,
                          e_source_get_display_name (parent), e_source_get_uid (parent));
}

/* Ein/Aus: nur GcalCalendar:visible; gespeichert wird in gcal-manager.c (Einstellungen der App). */
static void
on_calendar_toggled (GtkListItem    *item,
                     GtkCheckButton *button)
{
  GcalCalendar *calendar = gtk_list_item_get_item (item);

  if (calendar == NULL || gcal_calendar_get_visible (calendar) == gtk_check_button_get_active (button))
    return;

  g_debug ("Kalender: %s sichtbar %d", gcal_calendar_get_id (calendar), gtk_check_button_get_active (button));
  gcal_calendar_set_visible (calendar, gtk_check_button_get_active (button));
}

/* #374: die drei Ansichten der Reihe nach (Strg+Bild auf/ab) und die sichtbare. */
static const char *const view_names[] = { "month", "week", "day" };

static GtkWidget *
visible_view (RevolutionCalendarView *self)
{
  return adw_view_stack_get_visible_child (self->views);
}

/* Titel (Monat und Jahr, gcal-calendar-navigation-button.c:106 „%B %Y“) folgt dem Datum der sichtbaren Ansicht,
 * auch beim Scrollen des Monats (gcal-month-view.c:236-237). Die Monatsauswahl folgt im Monat nur, wenn der Monat
 * wechselt (sonst bliebe der gewählte Tag nicht stehen), in Woche und Tag dem Datum. */
static void
on_active_date_changed (RevolutionCalendarView *self)
{
  GtkWidget *view = visible_view (self);
  GDateTime *date = gcal_view_get_date (GCAL_VIEW (view));
  g_autoptr (GDateTime) selected = gtk_calendar_get_date (self->date_chooser);
  g_autofree char *title = g_date_time_format (date, "%B %Y");
  g_autofree char *day = g_date_time_format (date, "%F");

  gtk_label_set_label (self->month_title, title);
  g_debug ("Kalender: Ansicht %s Datum %s", adw_view_stack_get_visible_child_name (self->views), day);
  if (view == self->month_view ? g_date_time_get_month (date) != g_date_time_get_month (selected) ||
                                 g_date_time_get_year (date) != g_date_time_get_year (selected)
                               : gcal_date_time_compare_date (date, selected) != 0)
    gtk_calendar_set_date (self->date_chooser, date);
}

/* Datum zeigen: im Monat den 1. des Monats, dann ist die erste Zeile die Woche des Monatsersten wie im Design
 * (kalender-monat-hell.png) und wie nach vor/zurück (gcal_month_view_get_next_date); in Woche und Tag den Tag. */
static void
show_date (RevolutionCalendarView *self,
           GDateTime              *date)
{
  GtkWidget *view = visible_view (self);
  GDateTime *current = gcal_view_get_date (GCAL_VIEW (view));

  if (view == self->month_view)
    {
      g_autoptr (GDateTime) first = g_date_time_new_local (g_date_time_get_year (date), g_date_time_get_month (date), 1, 0, 0, 0);

      if (gcal_date_time_compare_date (first, current) != 0)
        g_object_set (view, "active-date", first, NULL); /* mit notify::active-date */
    }
  else if (gcal_date_time_compare_date (date, current) != 0)
    {
      g_object_set (view, "active-date", date, NULL);
    }
}

static void
on_day_selected (RevolutionCalendarView *self)
{
  g_autoptr (GDateTime) selected = gtk_calendar_get_date (self->date_chooser);

  show_date (self, selected);
}

/* Umschalten Monat | Woche | Tag: die neue Ansicht zeigt den gewählten Tag (gcal-window.c hält ein gemeinsames
 * active-date für alle Ansichten). */
static void
on_view_changed (RevolutionCalendarView *self)
{
  on_day_selected (self);
  on_active_date_changed (self);
}

/* calendar.previous / calendar.next / calendar.today wie gcal-window.c switch_prev_view,
 * switch_next_view und win.today. */
static void
on_previous (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  g_autoptr (GDateTime) date = gcal_view_get_previous_date (GCAL_VIEW (visible_view (self)));

  g_object_set (visible_view (self), "active-date", date, NULL);
}

static void
on_next (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  g_autoptr (GDateTime) date = gcal_view_get_next_date (GCAL_VIEW (visible_view (self)));

  g_object_set (visible_view (self), "active-date", date, NULL);
}

static void
on_today (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();

  show_date (self, now);
  gtk_calendar_set_date (self->date_chooser, now);
}

/* calendar.switch-view (Strg+Bild ab/auf): nächste bzw. vorige Ansicht wie gcal-window.c change-view. */
static void
on_switch_view (GSimpleAction *action G_GNUC_UNUSED,
                GVariant      *parameter,
                gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  const char *name = adw_view_stack_get_visible_child_name (self->views);
  int index = 0;

  while (index < (int) G_N_ELEMENTS (view_names) - 1 && g_strcmp0 (view_names[index], name) != 0)
    index++;
  index = CLAMP (index + g_variant_get_int32 (parameter), 0, (int) G_N_ELEMENTS (view_names) - 1);
  adw_view_stack_set_visible_child_name (self->views, view_names[index]);
}

/* ------------------------------------------------------------------ #375 Termine */

/* Fehler als AdwBanner über der Ansicht (Architektur 3, wie contacts-view.c show_error): kurzer Titel, Einzelheiten im
 * Tooltip und im Log. */
static void
on_manager_error (RevolutionCalendarView *self,
                  const char             *title,
                  const char             *detail)
{
  g_debug ("Kalender: Banner: %s: %s", title, detail);
  adw_banner_set_title (self->banner, title);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->banner), detail);
  adw_banner_set_revealed (self->banner, TRUE);
}

static void
on_banner_close (RevolutionCalendarView *self)
{
  g_debug ("Kalender: Banner ausgeblendet");
  adw_banner_set_revealed (self->banner, FALSE);
}

static void
present_editor (RevolutionCalendarView *self,
                GcalEvent              *event,
                gboolean                new_event)
{
  g_debug ("Kalender: Editor %s %s %s", new_event ? "neu" : "bearbeiten",
           gcal_calendar_get_id (gcal_event_get_calendar (event)), new_event ? "-" : gcal_event_get_uid (event));
  adw_dialog_present (ADW_DIALOG (self->editor), GTK_WIDGET (self));
  gcal_event_editor_dialog_set_event (self->editor, event, new_event);
}

/* Neuer Termin im Standardkalender (gcal_manager_get_default_calendar: nur gelesen, sonst der erste schreibbare). */
static void
new_event (RevolutionCalendarView *self,
           GDateTime              *start,
           GDateTime              *end)
{
  GcalCalendar *calendar = gcal_manager_get_default_calendar (gcal_context_get_manager (self->context));
  g_autoptr (ECalComponent) comp = NULL;
  g_autoptr (GcalEvent) event = NULL;

  if (calendar == NULL)
    {
      on_manager_error (self, _("No writable calendar"), _("Add a calendar in Online Accounts."));
      return;
    }
  comp = build_component_from_details ("", start, end);
  event = gcal_event_new (calendar, comp, NULL);
  present_editor (self, event, TRUE);
}

/* Doppelklick bzw. Ziehen im Monat oder Raster (GcalView::create-event, ::create-event-detailed): gleich der Editor
 * mit dem Bereich wie gcal-window.c create_event_detailed_cb (ohne Schnell-Popover gcal-quick-add-popover). */
static void
on_create_event (GcalView               *view,
                 GcalRange              *range,
                 RevolutionCalendarView *self)
{
  g_autoptr (GDateTime) start = gcal_range_get_start (range);
  g_autoptr (GDateTime) end = gcal_range_get_end (range);

  new_event (self, start, end);
  gcal_view_clear_marks (view);
}

static void
on_create_event_at (GcalView               *view,
                    GcalRange              *range,
                    gdouble                 x G_GNUC_UNUSED,
                    gdouble                 y G_GNUC_UNUSED,
                    RevolutionCalendarView *self)
{
  on_create_event (view, range, self);
}

/* calendar.new-event („+“, Strg+N, „Neuer Termin …“ im +N-Popover mit dessen Tag): ganztägig am Tag wie
 * gcal-window.c on_window_new_event_cb; ohne Tag der gewählte Tag der Monatsauswahl. */
static void
on_new_event (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter,
              gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  const char *text = g_variant_get_string (parameter, NULL);
  g_autoptr (GDateTime) day = *text != '\0' ? g_date_time_new_from_iso8601 (text, NULL) : NULL;
  g_autoptr (GDateTime) start = NULL;
  g_autoptr (GDateTime) end = NULL;

  if (day == NULL)
    day = gtk_calendar_get_date (self->date_chooser);
  start = g_date_time_new_utc (g_date_time_get_year (day), g_date_time_get_month (day),
                               g_date_time_get_day_of_month (day), 0, 0, 0);
  end = g_date_time_add_days (start, 1);
  new_event (self, start, end);
}

static void
on_preview (GcalEventWidget        *event_widget,
            GcalEventPreviewAction  action,
            gpointer                user_data)
{
  RevolutionCalendarView *self = user_data;

  g_clear_weak_pointer (&self->preview);
  if (action == GCAL_EVENT_PREVIEW_ACTION_EDIT)
    present_editor (self, gcal_event_widget_get_event (event_widget), FALSE);
}

/* Klick auf einen Termin: Popover (gcal-window.c event_activated). */
static void
on_event_activated (GcalView               *view G_GNUC_UNUSED,
                    GcalEventWidget        *event_widget,
                    RevolutionCalendarView *self)
{
  g_debug ("Kalender: Popover %s", gcal_event_get_uid (gcal_event_widget_get_event (event_widget)));
  g_set_weak_pointer (&self->preview, event_widget);
  gcal_event_widget_show_preview (event_widget, on_preview, self);

  /* Entf im Popover: eigene Fläche, Tasten erreichen die Kürzel der Ansicht nicht (gemessen, tests/kalender.sh). */
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (event_widget)); child; child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_POPOVER (child))
      {
        GtkEventController *shortcuts = gtk_shortcut_controller_new ();

        gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                              gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Delete, 0),
                                                                gtk_named_action_new ("calendar.delete-event")));
        gtk_widget_add_controller (child, shortcuts);
      }
}

/* Termin-Widgets aller drei Ansichten aus- bzw. einblenden (gcal-window.c hide_widget/show_widget). */
static void
show_event_widgets (RevolutionCalendarView *self,
                    GcalEvent              *event,
                    GcalRecurrenceModType   mod,
                    gboolean                visible)
{
  GtkWidget *views[] = { self->month_view, self->week_view, self->day_view };

  for (guint i = 0; i < G_N_ELEMENTS (views); i++)
    {
      g_autoptr (GList) widgets = gcal_view_get_children_by_uuid (GCAL_VIEW (views[i]), mod, gcal_event_get_uid (event));

      for (GList *l = widgets; l != NULL; l = l->next)
        gtk_widget_set_visible (l->data, visible);
    }
}

#define DELETE_SECONDS 5

static void
pending_free (PendingDelete *pending)
{
  g_clear_handle_id (&pending->timeout_id, g_source_remove);
  g_clear_object (&pending->event);
  g_clear_object (&pending->toast);
  g_free (pending);
}

/* Nach dem Schreiben: Anwendung freigeben; bei Fehler den Termin wieder zeigen (Banner kommt vom GcalManager). */
static void
on_removed (GObject      *source_object G_GNUC_UNUSED,
            GAsyncResult *result,
            gpointer      user_data)
{
  PendingDelete *pending = user_data;
  g_autoptr (RevolutionCalendarView) self = g_weak_ref_get (&pending->view);
  GApplication *application = g_application_get_default ();

  if (!g_task_propagate_boolean (G_TASK (result), NULL) && self != NULL)
    show_event_widgets (self, pending->event, pending->mod, TRUE);
  g_weak_ref_clear (&pending->view);
  pending_free (pending);
  if (application != NULL)
    g_application_release (application);
}

/* Vorgemerktes Löschen jetzt ausführen (Ablauf, nächstes Löschen, Fenster schließen; contacts-view.c execute_pending). */
static void
execute_pending (RevolutionCalendarView *self)
{
  PendingDelete *pending = g_steal_pointer (&self->pending);
  GApplication *application = g_application_get_default ();

  if (pending == NULL)
    return;
  g_clear_handle_id (&pending->timeout_id, g_source_remove);
  adw_toast_dismiss (pending->toast);
  g_debug ("Kalender: Löschen jetzt %s %s", gcal_calendar_get_id (gcal_event_get_calendar (pending->event)),
           gcal_event_get_uid (pending->event));
  if (application != NULL)
    g_application_hold (application);
  g_weak_ref_init (&pending->view, self);
  gcal_manager_remove_event (gcal_context_get_manager (self->context), pending->event, pending->mod, on_removed, pending);
}

static gboolean
on_delete_timeout (gpointer user_data)
{
  RevolutionCalendarView *self = user_data;

  self->pending->timeout_id = 0;
  execute_pending (self);
  return G_SOURCE_REMOVE;
}

static void
on_undo (RevolutionCalendarView *self)
{
  PendingDelete *pending = g_steal_pointer (&self->pending);

  if (pending == NULL)
    return;
  g_debug ("Kalender: Löschen rückgängig %s, nichts geschrieben", gcal_event_get_uid (pending->event));
  show_event_widgets (self, pending->event, pending->mod, TRUE);
  pending_free (pending);
}

/* Löschen ohne Rückfrage mit Toast „Termin gelöscht / Rückgängig“ (gcal-window.c:973-1001, Kontakte #370). */
static void
on_remove_event (GcalEventEditorDialog  *dialog G_GNUC_UNUSED,
                 GcalEvent              *event,
                 GcalRecurrenceModType   mod,
                 RevolutionCalendarView *self)
{
  GtkWidget *overlay = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY);
  PendingDelete *pending;

  execute_pending (self);
  pending = g_new0 (PendingDelete, 1);
  pending->event = g_object_ref (event);
  pending->mod = mod;
  pending->toast = adw_toast_new (_("Event deleted"));
  adw_toast_set_button_label (pending->toast, _("_Undo"));
  adw_toast_set_timeout (pending->toast, DELETE_SECONDS);
  adw_toast_set_priority (pending->toast, ADW_TOAST_PRIORITY_HIGH);
  g_signal_connect_object (pending->toast, "button-clicked", G_CALLBACK (on_undo), self, G_CONNECT_SWAPPED);
  pending->timeout_id = g_timeout_add_seconds (DELETE_SECONDS, on_delete_timeout, self);
  self->pending = pending;
  g_debug ("Kalender: Löschen vorgemerkt %s %s mod %d „%s“, in %d s", gcal_calendar_get_id (gcal_event_get_calendar (event)),
           gcal_event_get_uid (event), mod, gcal_event_get_summary (event), DELETE_SECONDS);
  show_event_widgets (self, event, mod, FALSE);
  if (overlay != NULL)
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), g_object_ref (pending->toast));
}

static void
on_delete_response (GcalEvent             *event,
                    GcalRecurrenceModType  mod,
                    gpointer               user_data)
{
  if (mod != GCAL_RECURRENCE_MOD_NONE)
    on_remove_event (NULL, event, mod, user_data);
}

/* calendar.delete-event (Entf): Termin mit offenem Popover, sonst der Termin mit dem Fokus; nicht in nur lesenden
 * Kalendern. Wiederholung: Abfrage wie der Editor (gcal-event-editor-dialog.c on_delete_row_activated_cb). */
static void
on_delete_event (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter G_GNUC_UNUSED,
                 gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  GtkWidget *widget = GTK_WIDGET (self->preview);
  g_autoptr (GcalEvent) event = NULL;

  if (widget == NULL)
    {
      GtkWidget *focus = gtk_root_get_focus (gtk_widget_get_root (GTK_WIDGET (self)));

      widget = focus != NULL ? gtk_widget_get_ancestor (focus, GCAL_TYPE_EVENT_WIDGET) : NULL;
    }
  if (widget == NULL)
    return;
  event = g_object_ref (gcal_event_widget_get_event (GCAL_EVENT_WIDGET (widget)));
  if (gcal_calendar_is_read_only (gcal_event_get_calendar (event)))
    return;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL; child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_POPOVER (child))
      gtk_popover_popdown (GTK_POPOVER (child));
  /* #452: auch eine geänderte Ausnahme (RECURRENCE-ID ohne RRULE) gehört zur Serie. */
  if (gcal_event_has_recurrence (event) || e_cal_component_is_instance (gcal_event_get_component (event)))
    gcal_utils_ask_recurrence_modification_type (GTK_WIDGET (self), event, TRUE, on_delete_response, self);
  else
    on_remove_event (NULL, event, GCAL_RECURRENCE_MOD_THIS_ONLY, self);
}

/* Fenster schließen wie Beenden: vorgemerktes Löschen sofort, die Anwendung hält sich bis zum Ende (execute_pending). */
static gboolean
on_close_request (RevolutionCalendarView *self)
{
  if (self->pending != NULL)
    g_debug ("Kalender: Fenster geschlossen, vorgemerktes Löschen jetzt");
  execute_pending (self);
  return FALSE;
}

/* Kalender verbinden erst, wenn die Seite erscheint (wie die Adressbücher, contacts-view.c on_map):
 * GcalManager lädt die Kalender der Registry, je Kalender verbindet GIO den ECalClient im Thread
 * (gcal-calendar.c gcal_calendar_new), die Timeline startet je Kalender eine ECalClientView im
 * eigenen Thread (gcal-calendar-monitor.c). Die Monatsansicht ist Abonnent der Timeline wie in
 * gcal-window.c:296-308. */
static void
on_map (RevolutionCalendarView *self)
{
  GcalManager *manager = gcal_context_get_manager (self->context);
  g_autolist (ESource) sources = NULL;

  if (self->started || self->registry == NULL)
    return;

  self->started = TRUE;
  sources = e_source_registry_list_enabled (self->registry, E_SOURCE_EXTENSION_CALENDAR);
  g_debug ("Kalender: Start mit %u Kalendern der Registry", g_list_length (sources));
  gcal_context_startup (self->context, self->registry);
  gtk_sort_list_model_set_model (self->calendar_sorted, gcal_manager_get_calendars_model (manager));
  g_signal_connect_object (manager, "calendar-added", G_CALLBACK (on_calendar_added), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (manager, "calendar-changed", G_CALLBACK (recalculate_calendar_colors_css), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (manager, "calendar-removed", G_CALLBACK (recalculate_calendar_colors_css), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (manager, "error", G_CALLBACK (on_manager_error), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (gtk_widget_get_root (GTK_WIDGET (self)), "close-request", G_CALLBACK (on_close_request), self,
                           G_CONNECT_SWAPPED);
  gcal_timeline_add_subscriber (gcal_manager_get_timeline (manager), GCAL_TIMELINE_SUBSCRIBER (self->month_view));
  gcal_timeline_add_subscriber (gcal_manager_get_timeline (manager), GCAL_TIMELINE_SUBSCRIBER (self->week_view));
  gcal_timeline_add_subscriber (gcal_manager_get_timeline (manager), GCAL_TIMELINE_SUBSCRIBER (self->day_view));
}

void
revolution_calendar_view_set_registry (RevolutionCalendarView *self,
                                       ESourceRegistry        *registry)
{
  g_return_if_fail (REVOLUTION_IS_CALENDAR_VIEW (self));
  g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
  g_return_if_fail (self->registry == NULL);

  self->registry = g_object_ref (registry);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    on_map (self);
}

static void
revolution_calendar_view_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  RevolutionCalendarView *self = REVOLUTION_CALENDAR_VIEW (object);

  if (prop_id == PROP_STACK)
    g_value_set_object (value, self->stack);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_calendar_view_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  RevolutionCalendarView *self = REVOLUTION_CALENDAR_VIEW (object);

  if (prop_id == PROP_STACK)
    g_set_object (&self->stack, g_value_get_object (value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_calendar_view_dispose (GObject *object)
{
  RevolutionCalendarView *self = REVOLUTION_CALENDAR_VIEW (object);

  execute_pending (self);
  g_clear_weak_pointer (&self->preview);
  if (self->editor != NULL)
    adw_dialog_force_close (ADW_DIALOG (self->editor));
  g_clear_object (&self->editor);

  if (self->context != NULL && self->started)
    {
      GcalTimeline *timeline = gcal_manager_get_timeline (gcal_context_get_manager (self->context));

      gcal_timeline_remove_subscriber (timeline, GCAL_TIMELINE_SUBSCRIBER (self->month_view));
      gcal_timeline_remove_subscriber (timeline, GCAL_TIMELINE_SUBSCRIBER (self->week_view));
      gcal_timeline_remove_subscriber (timeline, GCAL_TIMELINE_SUBSCRIBER (self->day_view));
    }

  if (self->colors != NULL)
    gtk_style_context_remove_provider_for_display (gtk_widget_get_display (GTK_WIDGET (self)),
                                                   GTK_STYLE_PROVIDER (self->colors));
  g_clear_object (&self->colors);

  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_CALENDAR_VIEW);
  g_clear_object (&self->stack);
  g_clear_object (&self->context);
  g_clear_object (&self->registry);

  G_OBJECT_CLASS (revolution_calendar_view_parent_class)->dispose (object);
}

static void
revolution_calendar_view_class_init (RevolutionCalendarViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = revolution_calendar_view_get_property;
  object_class->set_property = revolution_calendar_view_set_property;
  object_class->dispose = revolution_calendar_view_dispose;

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (GCAL_TYPE_MONTH_VIEW);
  g_type_ensure (GCAL_TYPE_WEEK_VIEW);
  g_type_ensure (GCAL_TYPE_CALENDAR);
  g_type_ensure (E_TYPE_SOURCE);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/calendar-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, views);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, calendar_sorted);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, month_view);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_view);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, day_view);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, date_chooser);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, month_title);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, app_switcher);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, banner);
  gtk_widget_class_bind_template_callback (widget_class, on_event_activated);
  gtk_widget_class_bind_template_callback (widget_class, on_banner_close);
  gtk_widget_class_bind_template_callback (widget_class, on_create_event);
  gtk_widget_class_bind_template_callback (widget_class, on_create_event_at);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
  gtk_widget_class_bind_template_callback (widget_class, on_day_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_calendar_toggled);
  gtk_widget_class_bind_template_callback (widget_class, calendar_color_name);
  gtk_widget_class_bind_template_callback (widget_class, calendar_section);
}

static void
revolution_calendar_view_init (RevolutionCalendarView *self)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) view_action = NULL;
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  /* #373: ein GcalContext je Ansicht (GNOME Calendar: einer je Anwendung, gcal-application.c);
   * Kalender verbinden erst in on_map. */
  self->context = gcal_context_new ();
  g_object_set (self->month_view, "context", self->context, NULL);
  /* #374: Woche und Tag brauchen ein Datum, bevor sie die Timeline abonnieren (get_range). */
  g_object_set (self->week_view, "context", self->context, "active-date", now, NULL);
  g_object_set (self->day_view, "context", self->context, "active-date", now, NULL);
  /* #375: ein Editor je Ansicht wie GcalWindow (gcal-window.blp event_editor). */
  self->editor = g_object_ref_sink (GCAL_EVENT_EDITOR_DIALOG (gcal_event_editor_dialog_new ()));
  g_object_set (self->editor, "context", self->context, NULL);
  g_signal_connect_object (self->editor, "remove-event", G_CALLBACK (on_remove_event), self, G_CONNECT_DEFAULT);
  self->colors = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (gtk_widget_get_display (GTK_WIDGET (self)),
                                              GTK_STYLE_PROVIDER (self->colors),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_signal_connect_swapped (self->month_view, "notify::active-date", G_CALLBACK (on_active_date_changed), self);
  g_signal_connect_swapped (self->week_view, "notify::active-date", G_CALLBACK (on_active_date_changed), self);
  g_signal_connect_swapped (self->day_view, "notify::active-date", G_CALLBACK (on_active_date_changed), self);
  g_signal_connect_swapped (self->views, "notify::visible-child-name", G_CALLBACK (on_view_changed), self);
  on_today (NULL, NULL, self);
  on_active_date_changed (self);
  g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);

  /* calendar.view für Alt+1/2/3 wie win.view in main.c;
   * calendar.sidebar für F9 (gcal-window.blp:181-188). */
  view_action = g_property_action_new ("view", self->views, "visible-child-name");
  sidebar_action = g_property_action_new ("sidebar", self->split, "show-sidebar");
  g_action_map_add_action_entries (G_ACTION_MAP (group), calendar_actions, G_N_ELEMENTS (calendar_actions), self);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (view_action));
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (sidebar_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "calendar", G_ACTION_GROUP (group));

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}
