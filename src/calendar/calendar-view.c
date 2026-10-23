/* calendar-view.c – Kalender-Ansicht (Prototyp #339)
 *
 * Vorlage: mail/mail-view.c (Template-Klasse mit Eigenschaft „stack“).
 * Oberfläche in calendar-view.blp. Monatsraster, Wochenkopf und Wochenraster
 * sind in GNOME Calendar 50.0 eigene C-Widgets (GcalMonthView, GcalWeekHeader,
 * GcalWeekGrid, GcalWeekHourBar); hier wird nur ihr Aussehen aus Standard-Widgets
 * (Gtk.Label, Gtk.Separator) in Gtk.Grid nachgebildet, ab dem heutigen Tag
 * und ohne Termine (#342; Termine, Blättern und Kalender folgen in M7).
 * Aktionen ohne Funktion sind nicht registriert, ihre Knöpfe sind ausgegraut.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "calendar-sample.h"
#include "calendar-view.h"

#define MONTH_WEEKS    5   /* gcal-month-view.c:39 N_ROWS_PER_PAGE */
#define MONTH_SLOTS    2   /* Terminzeilen je Monatszelle */
#define FIRST_HOUR     7   /* Woche zeigt fest 07–21 Uhr */
#define LAST_HOUR      21
#define HOUR_HEIGHT    40  /* = Höhe eines einstündigen Termins (Gtk.Button); 14 Stunden passen in 1200×760 */

struct _RevolutionCalendarView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  AdwOverlaySplitView *split;
  AdwViewStack        *views;
  GtkNoSelection      *calendar_model;
  GtkBox              *month_weeks;
  GtkGrid             *week_header;
  GtkGrid             *week_grid;
  GtkBox              *hour_bar;
  GtkLabel            *month_title;
  GtkCssProvider      *colors;
  GtkStack            *header_stack;
  GtkSearchEntry      *search_entry;
  GtkToggleButton     *search_button;
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

static const GActionEntry calendar_actions[] = {
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

/* Monat: je Woche ein Gtk.Grid mit Tagesnummer und MONTH_SLOTS Terminzeilen.
 * Trennlinien liegen als Gtk.Separator in denselben Zellen (Gtk.Grid erlaubt
 * Überlappung). Tag 1 zeigt den Monatsnamen, heute ist hervorgehoben
 * (gcal-month-cell.c:85-125, 540-570). */
static void
fill_month (RevolutionCalendarView *self,
            GDateTime              *today)
{
  g_autoptr (GDateTime) month_first = g_date_time_add_days (today, 1 - g_date_time_get_day_of_month (today));
  g_autoptr (GDateTime) first = week_start (month_first);
  /* Gerundet: über eine Zeitumstellung hat ein Tag 23 oder 25 Stunden. */
  int today_index = (g_date_time_difference (today, first) + G_TIME_SPAN_DAY / 2) / G_TIME_SPAN_DAY;
  GtkGrid *weeks[MONTH_WEEKS];

  /* Fünf Wochen ab der Woche des Monatsersten; liegt heute in einer sechsten
   * Woche, beginnt das Raster eine Woche später. */
  if (today_index >= MONTH_WEEKS * 7)
    {
      GDateTime *later = g_date_time_add_weeks (first, 1);

      g_date_time_unref (first);
      first = later;
      today_index -= 7;
    }

  for (int week = 0; week < MONTH_WEEKS; week++)
    {
      GtkWidget *filler = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

      weeks[week] = GTK_GRID (gtk_grid_new ());
      gtk_grid_set_column_homogeneous (weeks[week], TRUE);
      gtk_box_append (self->month_weeks, GTK_WIDGET (weeks[week]));

      if (week > 0)
        gtk_grid_attach (weeks[week], separator (GTK_ORIENTATION_HORIZONTAL), 0, 0, 7, 1);

      /* Freier Platz unten in der Zelle. */
      gtk_widget_set_vexpand (filler, TRUE);
      gtk_grid_attach (weeks[week], filler, 0, MONTH_SLOTS + 1, 7, 1);

      for (int column = 0; column < 7; column++)
        {
          int day = week * 7 + column;
          g_autoptr (GDateTime) date = g_date_time_add_days (first, day);
          int day_of_month = g_date_time_get_day_of_month (date);
          g_autofree char *text = day_of_month == 1
                                  ? g_date_time_format (date, "%B")
                                  : g_strdup_printf ("%d", day_of_month);
          GtkWidget *label = gtk_label_new (text);

          gtk_label_set_xalign (GTK_LABEL (label), 0);
          gtk_widget_set_margin_start (label, 12);
          gtk_widget_set_margin_top (label, 6);
          gtk_widget_set_margin_bottom (label, 4);
          gtk_widget_add_css_class (label, "numeric");
          /* #402: nur halbfett statt .heading (style.css .calendar-emphasis). */
          if (day_of_month == 1 || day == today_index)
            gtk_widget_add_css_class (label, "calendar-emphasis");
          if (day == today_index)
            gtk_widget_add_css_class (label, "accent");
          gtk_grid_attach (weeks[week], label, column, 0, 1, 1);

          if (column > 0)
            gtk_grid_attach (weeks[week], separator (GTK_ORIENTATION_VERTICAL), column, 0, 1, MONTH_SLOTS + 2);
        }
    }
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

/* Kalenderfarben sind Nutzerdaten (Briefing 3, Punkt 2): je Kalender eine
 * Farbklasse, erzeugt aus der Farbe des Kalenders, wie GNOME Calendar
 * recalculate_calendar_colors_css (gcal-window.c:345-372). Termine getönt mit
 * farbiger Kante (events.css:5-43), Ankreuzfeld in Kalenderfarbe.
 * #402: Tönung auf dem Fensterhintergrund (Hauptbereich wie Mail ohne .view),
 * dezent getönt (14 %, Runde 2 nach Vorbild 19), Kante 3 px. */
static void
load_colors (RevolutionCalendarView *self)
{
  GListModel *model = gtk_no_selection_get_model (self->calendar_model);
  g_autoptr (GString) css = g_string_new (NULL);

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (GObject) calendar = g_list_model_get_item (model, i);
      g_autofree char *color = NULL;
      g_auto (GStrv) classes = NULL;

      g_object_get (calendar, "color", &color, "css-classes", &classes, NULL);
      g_string_append_printf (css,
                              "checkbutton.%1$s > check { box-shadow: inset 0 0 0 2px %2$s; }\n"
                              "checkbutton.%1$s > check:checked { background-color: %2$s; color: var(--accent-fg-color); box-shadow: none; }\n"
                              "button.%1$s { background-color: color-mix(in srgb, %2$s 14%%, var(--window-bg-color)); box-shadow: inset 3px 0 %2$s; }\n",
                              classes[0], color);
    }

  self->colors = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (self->colors, css->str);
  gtk_style_context_add_provider_for_display (gtk_widget_get_display (GTK_WIDGET (self)),
                                              GTK_STYLE_PROVIDER (self->colors),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
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

  if (self->colors != NULL)
    gtk_style_context_remove_provider_for_display (gtk_widget_get_display (GTK_WIDGET (self)),
                                                   GTK_STYLE_PROVIDER (self->colors));
  g_clear_object (&self->colors);

  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_CALENDAR_VIEW);
  g_clear_object (&self->stack);

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

  g_type_ensure (REVOLUTION_TYPE_SAMPLE_CALENDAR);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/calendar-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, views);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, calendar_model);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, month_weeks);
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
}

static void
revolution_calendar_view_init (RevolutionCalendarView *self)
{
  g_autoptr (GListStore) calendars = g_list_store_new (REVOLUTION_TYPE_SAMPLE_CALENDAR);
  g_autoptr (GDateTime) today = today_midnight ();
  g_autofree char *title = g_date_time_format (today, "%B %Y");
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) view_action = NULL;
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_no_selection_set_model (self->calendar_model, G_LIST_MODEL (calendars));
  load_colors (self);
  /* Titel = Monat und Jahr (gcal-calendar-navigation-button.c:106, „%B %Y“). */
  gtk_label_set_label (self->month_title, title);
  fill_month (self, today);
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
