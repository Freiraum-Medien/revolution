/* calendar-view.c – Kalender-Ansicht (Prototyp #339, #373)
 *
 * Vorlage: mail/mail-view.c (Template-Klasse mit Eigenschaft „stack“).
 * Oberfläche in calendar-view.blp. #373: Kalender und Termine über die aus GNOME Calendar 50.0
 * übernommene Schicht in src/calendar/gcal (GcalContext → GcalManager → GcalTimeline, je Kalender ein
 * GcalCalendarMonitor mit ECalClientView); Monat = GcalMonthView. Die Anbindung folgt GcalWindow
 * (gcal-window.c). Die Woche bildet bis #374 nur das Aussehen von GcalWeekHeader/GcalWeekGrid
 * aus Standard-Widgets nach (Prototyp #339), ab dem heutigen Tag und ohne Termine.
 * Aktionen ohne Funktion sind nicht registriert, ihre Knöpfe sind ausgegraut.
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
#include "gcal/gcal-month-view.h"
#include "gcal/gcal-timeline.h"
#include "gcal/gcal-timeline-subscriber.h"
#include "gcal/gcal-view.h"

#define FIRST_HOUR     7   /* Woche zeigt fest 07–21 Uhr */
#define LAST_HOUR      21
#define HOUR_HEIGHT    40  /* = Höhe eines einstündigen Termins (Gtk.Button); 14 Stunden passen in 1200×760 */

struct _RevolutionCalendarView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  AdwOverlaySplitView *split;
  AdwViewStack        *views;
  GtkSortListModel    *calendar_sorted;
  GtkWidget           *month_view;
  GtkCalendar         *date_chooser;
  GtkGrid             *week_header;
  GtkGrid             *week_grid;
  GtkBox              *hour_bar;
  GtkLabel            *month_title;
  GtkCssProvider      *colors;
  GtkStack            *header_stack;
  GtkSearchEntry      *search_entry;
  GtkToggleButton     *search_button;
  GcalContext         *context;
  ESourceRegistry     *registry;
  gboolean             started;
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
  if (gtk_toggle_button_get_active (self->search_button))
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

static const GActionEntry calendar_actions[] = {
  { "previous", on_previous, NULL, NULL, NULL, { 0 } },
  { "next", on_next, NULL, NULL, NULL, { 0 } },
  { "today", on_today, NULL, NULL, NULL, { 0 } },
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  { "toggle-sidebar", on_toggle_sidebar, NULL, NULL, NULL, { 0 } },
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

/* Heute, 0:00 Uhr Ortszeit. */
static GDateTime *
today_midnight (void)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();

  return g_date_time_new_local (g_date_time_get_year (now), g_date_time_get_month (now),
                                g_date_time_get_day_of_month (now), 0, 0, 0);
}

/* Montag der Woche, in der date liegt (Wochenbeginn Montag wie im Prototyp #339). */
static GDateTime *
week_start (GDateTime *date)
{
  return g_date_time_add_days (date, 1 - g_date_time_get_day_of_week (date));
}

static GtkWidget *
separator (GtkOrientation orientation)
{
  GtkWidget *widget = gtk_separator_new (orientation);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    gtk_widget_set_valign (widget, GTK_ALIGN_START);
  else
    gtk_widget_set_halign (widget, GTK_ALIGN_START);

  return widget;
}

/* Woche von heute: Kopf mit Wochentag und Tagesnummer (gcal-week-header.c:995-1027,
 * 1565-1590), darunter Platz für ganztägige Termine; Raster FIRST_HOUR–LAST_HOUR in halben
 * Stunden. */
static void
fill_week (RevolutionCalendarView *self,
           GDateTime              *today)
{
  g_autoptr (GDateTime) first = week_start (today);
  int today_column = g_date_time_get_day_of_week (today) - 1;
  int rows = (LAST_HOUR - FIRST_HOUR) * 2;

  for (int column = 0; column < 7; column++)
    {
      g_autoptr (GDateTime) date = g_date_time_add_days (first, column);
      g_autofree char *weekday = g_date_time_format (date, "%a");
      g_autofree char *weekday_upper = g_utf8_strup (weekday, -1);
      g_autofree char *number = g_strdup_printf ("%d", g_date_time_get_day_of_month (date));
      const char *emphasis = column == today_column ? "accent" : "dimmed";
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *name = gtk_label_new (weekday_upper);
      GtkWidget *day = gtk_label_new (number);

      gtk_widget_set_margin_start (box, 6);
      gtk_label_set_xalign (GTK_LABEL (name), 0);
      gtk_label_set_xalign (GTK_LABEL (day), 0);
      /* #402: leichte Typografie, Wochentag wie der Monatskopf (caption-heading),
       * Tagesnummer groß, aber nicht fett; nur heute halbfett (style.css). */
      gtk_widget_add_css_class (name, "caption-heading");
      gtk_widget_add_css_class (name, emphasis);
      gtk_widget_add_css_class (day, "title-2");
      gtk_widget_add_css_class (day, "calendar-day-number");
      gtk_widget_add_css_class (day, "numeric");
      gtk_widget_add_css_class (day, emphasis);
      if (column == today_column)
        gtk_widget_add_css_class (day, "calendar-emphasis");
      gtk_box_append (GTK_BOX (box), name);
      gtk_box_append (GTK_BOX (box), day);
      gtk_grid_attach (self->week_header, box, column, 0, 1, 1);

      gtk_grid_attach (self->week_grid, separator (GTK_ORIENTATION_VERTICAL), column, 0, 1, rows);
    }

  for (int hour = FIRST_HOUR; hour < LAST_HOUR; hour++)
    {
      g_autofree char *text = g_strdup_printf ("%02d:00", hour);
      GtkWidget *label = gtk_label_new (text);

      gtk_label_set_yalign (GTK_LABEL (label), 0);
      gtk_widget_set_size_request (label, -1, HOUR_HEIGHT - 4);
      gtk_widget_set_margin_top (label, 4);
      gtk_widget_set_margin_start (label, 6);
      gtk_widget_set_margin_end (label, 6);
      /* #402: Uhrzeit wie die Zeit in der Mail-Liste (caption, numeric, dimmed). */
      gtk_widget_add_css_class (label, "caption");
      gtk_widget_add_css_class (label, "dimmed");
      gtk_widget_add_css_class (label, "numeric");
      gtk_box_append (self->hour_bar, label);

      if (hour > FIRST_HOUR)
        gtk_grid_attach (self->week_grid, separator (GTK_ORIENTATION_HORIZONTAL), 0, (hour - FIRST_HOUR) * 2, 7, 1);
    }
}

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

/* Titel (Monat und Jahr, gcal-calendar-navigation-button.c:106 „%B %Y“) folgt dem Datum der Monatsansicht,
 * auch beim Scrollen (gcal-month-view.c:236-237); die Monatsauswahl nur, wenn der Monat wechselt (sonst
 * bliebe der gewählte Tag nicht stehen). */
static void
on_active_date_changed (RevolutionCalendarView *self)
{
  GDateTime *date = gcal_view_get_date (GCAL_VIEW (self->month_view));
  g_autoptr (GDateTime) selected = gtk_calendar_get_date (self->date_chooser);
  g_autofree char *title = g_date_time_format (date, "%B %Y");

  gtk_label_set_label (self->month_title, title);
  if (g_date_time_get_month (date) != g_date_time_get_month (selected) ||
      g_date_time_get_year (date) != g_date_time_get_year (selected))
    gtk_calendar_set_date (self->date_chooser, date);
}

/* Monat zeigen: Datum der Ansicht = 1. des Monats, die erste Zeile ist dann die Woche des Monatsersten wie im
 * Design (kalender-monat-hell.png) und wie nach vor/zurück (gcal_month_view_get_next_date). */
static void
show_month (RevolutionCalendarView *self,
            GDateTime              *date)
{
  g_autoptr (GDateTime) first = g_date_time_new_local (g_date_time_get_year (date), g_date_time_get_month (date), 1, 0, 0, 0);

  g_object_set (self->month_view, "active-date", first, NULL); /* mit notify::active-date */
}

static void
on_day_selected (RevolutionCalendarView *self)
{
  g_autoptr (GDateTime) selected = gtk_calendar_get_date (self->date_chooser);
  GDateTime *date = gcal_view_get_date (GCAL_VIEW (self->month_view));

  if (g_date_time_get_month (date) != g_date_time_get_month (selected) ||
      g_date_time_get_year (date) != g_date_time_get_year (selected))
    show_month (self, selected);
}

/* calendar.previous / calendar.next / calendar.today wie gcal-window.c switch_prev_view,
 * switch_next_view und win.today. */
static void
on_previous (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  g_autoptr (GDateTime) date = gcal_view_get_previous_date (GCAL_VIEW (self->month_view));

  g_object_set (self->month_view, "active-date", date, NULL);
}

static void
on_next (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  g_autoptr (GDateTime) date = gcal_view_get_next_date (GCAL_VIEW (self->month_view));

  g_object_set (self->month_view, "active-date", date, NULL);
}

static void
on_today (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  RevolutionCalendarView *self = user_data;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();

  show_month (self, now);
  gtk_calendar_set_date (self->date_chooser, now);
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
  gcal_timeline_add_subscriber (gcal_manager_get_timeline (manager), GCAL_TIMELINE_SUBSCRIBER (self->month_view));
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

  if (self->context != NULL && self->started)
    gcal_timeline_remove_subscriber (gcal_manager_get_timeline (gcal_context_get_manager (self->context)),
                                     GCAL_TIMELINE_SUBSCRIBER (self->month_view));

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
  g_type_ensure (GCAL_TYPE_CALENDAR);
  g_type_ensure (E_TYPE_SOURCE);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/calendar-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, views);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, calendar_sorted);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, month_view);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, date_chooser);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_header);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_grid);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, hour_bar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, month_title);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, search_button);
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
  g_autoptr (GDateTime) today = today_midnight ();
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) view_action = NULL;
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  /* #373: ein GcalContext je Ansicht (GNOME Calendar: einer je Anwendung, gcal-application.c);
   * Kalender verbinden erst in on_map. */
  self->context = gcal_context_new ();
  g_object_set (self->month_view, "context", self->context, NULL);
  self->colors = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (gtk_widget_get_display (GTK_WIDGET (self)),
                                              GTK_STYLE_PROVIDER (self->colors),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_signal_connect_swapped (self->month_view, "notify::active-date", G_CALLBACK (on_active_date_changed), self);
  on_today (NULL, NULL, self);
  on_active_date_changed (self);
  g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);
  fill_week (self, today);

  /* calendar.view für Alt+1/Alt+2 wie win.view in main.c;
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
