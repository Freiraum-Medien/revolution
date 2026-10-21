/* calendar-view.c – Kalender-Ansicht (Prototyp #339)
 *
 * Vorlage: mail/mail-view.c (Template-Klasse mit Eigenschaft „stack“).
 * Oberfläche in calendar-view.blp. Monatsraster, Wochenkopf und Wochenraster
 * sind in GNOME Calendar 50.0 eigene C-Widgets (GcalMonthView, GcalWeekHeader,
 * GcalWeekGrid, GcalWeekHourBar); hier wird nur ihr Aussehen aus Standard-Widgets
 * (Gtk.Label, Gtk.Button, Gtk.Separator) in Gtk.Grid nachgebildet, mit festen
 * Beispielterminen aus calendar-sample.c. Keine Kalenderlogik.
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
  GtkPopover          *event_popover;
  GtkLabel            *event_summary;
  GtkLabel            *event_when;
  GtkLabel            *event_notes;
  GtkWidget           *event_location_box;
  GtkLabel            *event_location;
  GtkCssProvider      *colors;
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

/* Platzhalter ohne Logik, nur damit Knöpfe und Kürzel nicht ausgegraut sind.
 * preferences, shortcuts, about: Platzhalter des Hauptmenüs wie in mail-view.c. */
static const GActionEntry calendar_actions[] = {
  { "today", NULL, NULL, NULL, NULL, { 0 } },
  { "previous", NULL, NULL, NULL, NULL, { 0 } },
  { "next", NULL, NULL, NULL, NULL, { 0 } },
  { "new-event", NULL, NULL, NULL, NULL, { 0 } },
  { "edit-event", NULL, NULL, NULL, NULL, { 0 } },
  { "search", NULL, NULL, NULL, NULL, { 0 } },
  { "toggle-sidebar", on_toggle_sidebar, NULL, NULL, NULL, { 0 } },
  { "preferences", NULL, NULL, NULL, NULL, { 0 } },
  { "shortcuts", NULL, NULL, NULL, NULL, { 0 } },
  { "about", NULL, NULL, NULL, NULL, { 0 } },
};

/* #402: Umschalter nur mit Symbolen, Tooltip = Titel der n-ten Stack-Seite.
 * Übernommen aus mail-view.c on_switcher_query_tooltip (src/mail/ ist für diesen
 * Task gesperrt, daher Kopie statt gemeinsamer Funktion). */
static gboolean
on_switcher_query_tooltip (RevolutionCalendarView *self,
                           int                     x,
                           int                     y,
                           gboolean                keyboard,
                           GtkTooltip             *tooltip,
                           GtkWidget              *switcher)
{
  g_autoptr (GtkSelectionModel) pages = NULL;
  g_autoptr (AdwViewStackPage) page = NULL;
  GtkWidget *button;
  GtkWidget *child;
  guint index = 0;

  if (keyboard)
    button = gtk_root_get_focus (gtk_widget_get_root (switcher));
  else
    button = gtk_widget_pick (switcher, x, y, GTK_PICK_DEFAULT);

  while (button != NULL && button != switcher && !GTK_IS_TOGGLE_BUTTON (button))
    button = gtk_widget_get_parent (button);

  if (button == NULL || button == switcher || self->stack == NULL)
    return FALSE;

  for (child = gtk_widget_get_first_child (switcher); child != NULL && child != button;
       child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_TOGGLE_BUTTON (child))
      index++;

  if (child == NULL)
    return FALSE;

  pages = adw_view_stack_get_pages (self->stack);
  page = g_list_model_get_item (G_LIST_MODEL (pages), index);
  if (page == NULL)
    return FALSE;

  gtk_tooltip_set_text (tooltip, adw_view_stack_page_get_title (page));
  return TRUE;
}

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

static GDateTime *
sample_date (guint day)
{
  g_autoptr (GDateTime) first = g_date_time_new_local (REVOLUTION_SAMPLE_FIRST_DAY, 0, 0, 0);

  return g_date_time_add_days (first, day);
}

/* Termin-Detail wie GcalEventPopover (gcal-event-popover.blp): Titel, Zeit,
 * Notizen, Ort. Das Popover wandert an den angeklickten Termin. */
static void
on_event_clicked (GtkButton              *button,
                  RevolutionCalendarView *self)
{
  const RevolutionSampleEvent *event = g_object_get_data (G_OBJECT (button), "event");

  gtk_label_set_label (self->event_summary, _(event->summary));
  gtk_label_set_label (self->event_when, _(event->when));
  gtk_label_set_label (self->event_notes, event->notes ? _(event->notes) : _("Keine Notizen"));
  if (event->notes)
    gtk_widget_remove_css_class (GTK_WIDGET (self->event_notes), "dimmed");
  else
    gtk_widget_add_css_class (GTK_WIDGET (self->event_notes), "dimmed");
  gtk_widget_set_visible (self->event_location_box, event->location != NULL);
  gtk_label_set_label (self->event_location, event->location ? _(event->location) : "");

  if (gtk_widget_get_parent (GTK_WIDGET (self->event_popover)) != NULL)
    gtk_widget_unparent (GTK_WIDGET (self->event_popover));
  gtk_widget_set_parent (GTK_WIDGET (self->event_popover), GTK_WIDGET (button));
  gtk_popover_popup (self->event_popover);
}

/* Termin als Knopf in Kalenderfarbe (Farbklasse siehe load_colors), Titel und
 * Beginn wie GcalEventWidget horizontal_box (gcal-event-widget.blp:64-78). */
static GtkWidget *
event_button (RevolutionCalendarView      *self,
              const RevolutionSampleEvent *event)
{
  g_autofree char *color_class = g_strdup_printf ("calendar-color-%u", event->calendar);
  g_autofree char *a11y = NULL;
  GtkWidget *button = gtk_button_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *summary = gtk_label_new (_(event->summary));

  gtk_widget_set_valign (box, GTK_ALIGN_START);
  gtk_label_set_xalign (GTK_LABEL (summary), 0);
  gtk_label_set_ellipsize (GTK_LABEL (summary), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (summary, TRUE);
  gtk_widget_add_css_class (summary, "body");
  gtk_box_append (GTK_BOX (box), summary);

  if (event->time != NULL)
    {
      GtkWidget *time = gtk_label_new (event->time);

      gtk_widget_add_css_class (time, "caption");
      gtk_widget_add_css_class (time, "dimmed");
      gtk_widget_add_css_class (time, "numeric");
      gtk_box_append (GTK_BOX (box), time);
    }

  a11y = g_strdup_printf ("%s, %s", _(event->summary), _(event->when));
  gtk_button_set_child (GTK_BUTTON (button), box);
  /* #402: ruhig, abgerundet, Schrift leichter (style.css .calendar-event). */
  gtk_widget_add_css_class (button, "calendar-event");
  gtk_widget_add_css_class (button, color_class);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, a11y,
                                  GTK_ACCESSIBLE_PROPERTY_HAS_POPUP, TRUE,
                                  -1);
  g_object_set_data (G_OBJECT (button), "event", (gpointer) event);
  g_signal_connect (button, "clicked", G_CALLBACK (on_event_clicked), self);

  return button;
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
fill_month (RevolutionCalendarView *self)
{
  const RevolutionSampleEvent *events;
  guint n_events;
  GtkGrid *weeks[MONTH_WEEKS];

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
          guint day = week * 7 + column;
          g_autoptr (GDateTime) date = sample_date (day);
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
          if (day_of_month == 1 || day == REVOLUTION_SAMPLE_TODAY)
            gtk_widget_add_css_class (label, "calendar-emphasis");
          if (day == REVOLUTION_SAMPLE_TODAY)
            gtk_widget_add_css_class (label, "accent");
          gtk_grid_attach (weeks[week], label, column, 0, 1, 1);

          if (column > 0)
            gtk_grid_attach (weeks[week], separator (GTK_ORIENTATION_VERTICAL), column, 0, 1, MONTH_SLOTS + 2);
        }
    }

  events = revolution_calendar_sample_events (&n_events);
  for (guint i = 0; i < n_events; i++)
    {
      const RevolutionSampleEvent *event = &events[i];
      GtkWidget *button = event_button (self, event);

      g_assert (event->day < MONTH_WEEKS * 7);
      g_assert (event->day % 7 + event->days <= 7);
      g_assert (event->slot < MONTH_SLOTS);

      gtk_widget_set_margin_start (button, 4);
      gtk_widget_set_margin_end (button, 4);
      gtk_widget_set_margin_bottom (button, 2);
      gtk_grid_attach (weeks[event->day / 7], button, event->day % 7, 1 + event->slot, event->days, 1);
    }
}

/* Woche 19.–25.10.: Kopf mit Wochentag und Tagesnummer (gcal-week-header.c:995-1027,
 * 1565-1590), ganztägige Termine darunter; Raster FIRST_HOUR–LAST_HOUR in halben
 * Stunden. */
static void
fill_week (RevolutionCalendarView *self)
{
  const RevolutionSampleEvent *events;
  guint n_events;
  int rows = (LAST_HOUR - FIRST_HOUR) * 2;

  for (int column = 0; column < 7; column++)
    {
      g_autoptr (GDateTime) date = sample_date (REVOLUTION_SAMPLE_WEEK + column);
      g_autofree char *weekday = g_date_time_format (date, "%a");
      g_autofree char *weekday_upper = g_utf8_strup (weekday, -1);
      g_autofree char *number = g_strdup_printf ("%d", g_date_time_get_day_of_month (date));
      const char *emphasis = REVOLUTION_SAMPLE_WEEK + column == REVOLUTION_SAMPLE_TODAY ? "accent" : "dimmed";
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
      if (REVOLUTION_SAMPLE_WEEK + column == REVOLUTION_SAMPLE_TODAY)
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

  events = revolution_calendar_sample_events (&n_events);
  for (guint i = 0; i < n_events; i++)
    {
      const RevolutionSampleEvent *event = &events[i];
      GtkWidget *button;

      if (event->day < REVOLUTION_SAMPLE_WEEK || event->day >= REVOLUTION_SAMPLE_WEEK + 7)
        continue;

      button = event_button (self, event);
      gtk_widget_set_margin_start (button, 3);
      gtk_widget_set_margin_end (button, 3);

      if (event->start < 0)
        {
          gtk_widget_set_margin_top (button, 6);
          gtk_widget_set_margin_bottom (button, 4);
          gtk_grid_attach (self->week_header, button, event->day - REVOLUTION_SAMPLE_WEEK, 1, event->days, 1);
        }
      else
        {
          g_assert (event->start >= FIRST_HOUR * 60 && event->end <= LAST_HOUR * 60);

          gtk_grid_attach (self->week_grid, button, event->day - REVOLUTION_SAMPLE_WEEK,
                           (event->start - FIRST_HOUR * 60) / 30, 1, (event->end - event->start) / 30);
        }
    }
}

/* Kalenderfarben sind Nutzerdaten (Briefing 3, Punkt 2): je Kalender eine
 * Farbklasse, erzeugt aus der Farbe des Kalenders, wie GNOME Calendar
 * recalculate_calendar_colors_css (gcal-window.c:345-372). Termine getönt mit
 * farbiger Kante (events.css:5-43), Ankreuzfeld in Kalenderfarbe.
 * #402: Tönung auf dem Fensterhintergrund (Hauptbereich wie Mail ohne .view),
 * etwas zurückgenommen (20 %), Kante 3 px. */
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
                              "checkbutton.%1$s > check:checked { background-color: %2$s; color: white; box-shadow: none; }\n"
                              "button.%1$s { background-color: color-mix(in srgb, %2$s 20%%, var(--window-bg-color)); box-shadow: inset 3px 0 %2$s; }\n",
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

  if (self->event_popover != NULL && gtk_widget_get_parent (GTK_WIDGET (self->event_popover)) != NULL)
    gtk_widget_unparent (GTK_WIDGET (self->event_popover));
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

  gtk_widget_class_set_template_from_resource (widget_class, "/org/revolution/Mail/calendar-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, views);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, calendar_model);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, month_weeks);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_header);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_grid);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, hour_bar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_popover);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_summary);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_when);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_notes);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_location_box);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_location);
  gtk_widget_class_bind_template_callback (widget_class, on_switcher_query_tooltip);
}

static void
revolution_calendar_view_init (RevolutionCalendarView *self)
{
  g_autoptr (GListModel) calendars = revolution_calendar_sample_calendars ();
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) view_action = NULL;
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_no_selection_set_model (self->calendar_model, calendars);
  load_colors (self);
  fill_month (self);
  fill_week (self);

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
