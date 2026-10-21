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
#define MONTH_SLOTS    2   /* Terminzeilen je Monatszelle, weitere als „+N“ */
#define HOUR_HEIGHT    48  /* Mindesthöhe je Stunde (Stundenleiste) */
#define FIRST_HOUR     7.5 /* oberer Rand der Wochenansicht beim ersten Anzeigen */

struct _RevolutionCalendarView
{
  AdwBreakpointBin  parent_instance;
  AdwViewStack     *stack;
  AdwViewStack     *views;
  GtkNoSelection   *calendar_model;
  GtkBox           *month_weeks;
  GtkGrid          *week_header;
  GtkGrid          *week_grid;
  GtkBox           *hour_bar;
  GtkScrolledWindow *week_scroller;
  GtkPopover       *event_popover;
  GtkLabel         *event_summary;
  GtkLabel         *event_when;
  GtkLabel         *event_notes;
  GtkWidget        *event_location_box;
  GtkLabel         *event_location;
  GtkCssProvider   *colors;
};

G_DEFINE_FINAL_TYPE (RevolutionCalendarView, revolution_calendar_view, ADW_TYPE_BREAKPOINT_BIN)

/* Platzhalter ohne Logik, nur damit Knöpfe und Kürzel nicht ausgegraut sind. */
static const GActionEntry calendar_actions[] = {
  { "today", NULL, NULL, NULL, NULL, { 0 } },
  { "previous", NULL, NULL, NULL, NULL, { 0 } },
  { "next", NULL, NULL, NULL, NULL, { 0 } },
  { "new-event", NULL, NULL, NULL, NULL, { 0 } },
  { "edit-event", NULL, NULL, NULL, NULL, { 0 } },
};

enum { PROP_0, PROP_STACK, N_PROPS };

static GDateTime *
sample_date (guint day)
{
  g_autoptr (GDateTime) first = g_date_time_new_local (REVOLUTION_SAMPLE_FIRST_DAY, 0, 0, 0);

  return g_date_time_add_days (first, day);
}

static char *
event_time_text (const RevolutionSampleEvent *event)
{
  return g_strdup_printf ("%02d:%02d – %02d:%02d",
                          event->start / 60, event->start % 60, event->end / 60, event->end % 60);
}

/* Termin-Detail wie GcalEventPopover (gcal-event-popover.blp): Titel, Zeit,
 * Notizen, Ort. Das Popover wandert an den angeklickten Termin. */
static void
on_event_clicked (GtkButton              *button,
                  RevolutionCalendarView *self)
{
  const RevolutionSampleEvent *event = g_object_get_data (G_OBJECT (button), "event");
  g_autoptr (GDateTime) date = sample_date (event->day);
  g_autofree char *day_text = g_date_time_format (date, _("%A, %-d. %B"));
  g_autofree char *when = NULL;

  if (event->start >= 0)
    {
      g_autofree char *time_text = event_time_text (event);

      when = g_strdup_printf (_("%s, %s"), day_text, time_text);
    }
  else if (event->days > 1)
    {
      g_autoptr (GDateTime) last = sample_date (event->day + event->days - 1);
      g_autofree char *last_text = g_date_time_format (last, _("%A, %-d. %B"));

      when = g_strdup_printf (_("%s bis %s"), day_text, last_text);
    }
  else
    {
      when = g_strdup_printf (_("%s, ganztägig"), day_text);
    }

  gtk_label_set_label (self->event_summary, _(event->summary));
  gtk_label_set_label (self->event_when, when);
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

/* Termin als Knopf in Kalenderfarbe (Farbklasse siehe load_colors).
 * Waagrecht wie GcalEventWidget horizontal_box, senkrecht wie vertical_box
 * (gcal-event-widget.blp:37-78). */
static GtkWidget *
event_button (RevolutionCalendarView      *self,
              const RevolutionSampleEvent *event,
              GtkOrientation               orientation)
{
  g_autofree char *color_class = g_strdup_printf ("calendar-color-%u", event->calendar);
  g_autofree char *time_text = event->start >= 0 ? event_time_text (event) : NULL;
  g_autofree char *a11y = NULL;
  GtkWidget *button = gtk_button_new ();
  GtkWidget *box = gtk_box_new (orientation, 4);
  GtkWidget *summary = gtk_label_new (_(event->summary));

  gtk_label_set_xalign (GTK_LABEL (summary), 0);
  gtk_label_set_ellipsize (GTK_LABEL (summary), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (summary, TRUE);
  gtk_widget_add_css_class (summary, "body");

  if (time_text != NULL)
    {
      /* Monat: nur Beginn (wie GcalEventWidget timestamp_label), Woche: Beginn – Ende. */
      g_autofree char *start_text = g_strndup (time_text, 5);
      GtkWidget *time = gtk_label_new (orientation == GTK_ORIENTATION_HORIZONTAL ? start_text : time_text);

      gtk_label_set_xalign (GTK_LABEL (time), 0);
      gtk_widget_add_css_class (time, "caption");
      gtk_widget_add_css_class (time, "dimmed");
      gtk_widget_add_css_class (time, "numeric");
      if (orientation == GTK_ORIENTATION_HORIZONTAL)
        {
          gtk_box_append (GTK_BOX (box), summary);
          gtk_box_append (GTK_BOX (box), time);
        }
      else
        {
          gtk_box_append (GTK_BOX (box), time);
          gtk_box_append (GTK_BOX (box), summary);
        }
      a11y = g_strdup_printf ("%s, %s", _(event->summary), time_text);
    }
  else
    {
      gtk_box_append (GTK_BOX (box), summary);
      a11y = g_strdup_printf (_("%s, ganztägig"), _(event->summary));
    }

  gtk_button_set_child (GTK_BUTTON (button), box);
  gtk_widget_add_css_class (button, color_class);
  gtk_widget_set_tooltip_text (button, a11y);
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

/* Monat: je Woche ein Gtk.Grid mit Tagesnummer, MONTH_SLOTS Terminzeilen und
 * einer Zeile „+N“ für weitere Termine (gcal-month-cell.blp:66-92, .c:631).
 * Trennlinien liegen als Gtk.Separator in denselben Zellen (Gtk.Grid erlaubt
 * Überlappung). Tag 1 zeigt den Monatsnamen, heute ist hervorgehoben
 * (gcal-month-cell.c:85-125, 540-570). */
static void
fill_month (RevolutionCalendarView *self)
{
  const RevolutionSampleEvent *events;
  guint n_events;
  guint hidden[MONTH_WEEKS * 7] = { 0, };
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
          if (day_of_month == 1 || day == REVOLUTION_SAMPLE_TODAY)
            gtk_widget_add_css_class (label, "heading");
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
      GtkWidget *button;

      if (event->slot >= MONTH_SLOTS)
        {
          hidden[event->day]++;
          continue;
        }

      button = event_button (self, event, GTK_ORIENTATION_HORIZONTAL);
      gtk_widget_set_margin_start (button, 4);
      gtk_widget_set_margin_end (button, 4);
      gtk_widget_set_margin_bottom (button, 2);
      gtk_grid_attach (weeks[event->day / 7], button, event->day % 7, 1 + event->slot, event->days, 1);
    }

  for (guint day = 0; day < G_N_ELEMENTS (hidden); day++)
    {
      g_autofree char *text = NULL;
      g_autofree char *tooltip = NULL;
      GtkWidget *label;

      if (hidden[day] == 0)
        continue;

      text = g_strdup_printf ("+%u", hidden[day]);
      tooltip = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u weiterer Termin", "%u weitere Termine", hidden[day]),
                                 hidden[day]);
      label = gtk_label_new (text);
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_widget_set_valign (label, GTK_ALIGN_START);
      gtk_widget_set_margin_start (label, 12);
      gtk_widget_set_tooltip_text (label, tooltip);
      gtk_accessible_update_property (GTK_ACCESSIBLE (label), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
      gtk_widget_add_css_class (label, "caption-heading");
      gtk_widget_add_css_class (label, "dimmed");
      gtk_grid_attach (weeks[day / 7], label, day % 7, MONTH_SLOTS + 1, 1, 1);
    }
}

/* Woche 19.–25.10.: Kopf mit Wochentag und Tagesnummer (gcal-week-header.c:995-1027,
 * 1565-1590), ganztägige Termine darunter; Raster mit 48 halben Stunden. */
static void
fill_week (RevolutionCalendarView *self)
{
  const RevolutionSampleEvent *events;
  guint n_events;

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
      gtk_widget_add_css_class (name, "heading");
      gtk_widget_add_css_class (name, emphasis);
      gtk_widget_add_css_class (day, "title-2");
      gtk_widget_add_css_class (day, "numeric");
      gtk_widget_add_css_class (day, emphasis);
      gtk_box_append (GTK_BOX (box), name);
      gtk_box_append (GTK_BOX (box), day);
      gtk_grid_attach (self->week_header, box, column, 0, 1, 1);

      gtk_grid_attach (self->week_grid, separator (GTK_ORIENTATION_VERTICAL), column, 0, 1, 48);
    }

  for (int hour = 0; hour < 24; hour++)
    {
      g_autofree char *text = g_strdup_printf ("%02d:00", hour);
      GtkWidget *label = gtk_label_new (text);

      gtk_label_set_yalign (GTK_LABEL (label), 0);
      gtk_widget_set_size_request (label, -1, HOUR_HEIGHT - 4);
      gtk_widget_set_margin_top (label, 4);
      gtk_widget_set_margin_start (label, 6);
      gtk_widget_set_margin_end (label, 6);
      gtk_widget_add_css_class (label, "dimmed");
      gtk_widget_add_css_class (label, "numeric");
      gtk_box_append (self->hour_bar, label);

      if (hour > 0)
        gtk_grid_attach (self->week_grid, separator (GTK_ORIENTATION_HORIZONTAL), 0, hour * 2, 7, 1);
    }

  events = revolution_calendar_sample_events (&n_events);
  for (guint i = 0; i < n_events; i++)
    {
      const RevolutionSampleEvent *event = &events[i];
      GtkWidget *button;

      if (event->day < REVOLUTION_SAMPLE_WEEK || event->day >= REVOLUTION_SAMPLE_WEEK + 7)
        continue;

      if (event->start < 0)
        {
          button = event_button (self, event, GTK_ORIENTATION_HORIZONTAL);
          gtk_widget_set_margin_top (button, 6);
          gtk_widget_set_margin_start (button, 2);
          gtk_widget_set_margin_end (button, 2);
          gtk_grid_attach (self->week_header, button, event->day - REVOLUTION_SAMPLE_WEEK, 1, event->days, 1);
        }
      else
        {
          button = event_button (self, event, GTK_ORIENTATION_VERTICAL);
          gtk_widget_set_margin_top (button, 1);
          gtk_widget_set_margin_bottom (button, 1);
          gtk_widget_set_margin_start (button, 3);
          gtk_widget_set_margin_end (button, 3);
          gtk_grid_attach (self->week_grid, button, event->day - REVOLUTION_SAMPLE_WEEK,
                           event->start / 30, 1, (event->end - event->start) / 30);
        }
    }
}

/* Beim ersten Anzeigen der Woche zum Vormittag scrollen (GNOME Calendar
 * scrollt zur aktuellen Uhrzeit). Erst nach der Größenzuteilung, sonst setzt
 * der Viewport den Wert zurück. */
static void
scroll_to_first_hour (gpointer user_data)
{
  GtkAdjustment *adjustment = user_data;

  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_upper (adjustment) * FIRST_HOUR / 24);
  g_object_unref (adjustment);
}

static void
on_week_adjustment_changed (GtkAdjustment *adjustment,
                            gpointer       user_data)
{
  if (gtk_adjustment_get_upper (adjustment) <= gtk_adjustment_get_page_size (adjustment))
    return;

  g_signal_handlers_disconnect_by_func (adjustment, on_week_adjustment_changed, user_data);
  g_idle_add_once (scroll_to_first_hour, g_object_ref (adjustment));
}

/* Kalenderfarben sind Nutzerdaten (Briefing 3, Punkt 2): je Kalender eine
 * Farbklasse, erzeugt aus der Farbe des Kalenders, wie GNOME Calendar
 * recalculate_calendar_colors_css (gcal-window.c:345-372). Termine getönt mit
 * farbiger Kante (events.css:5-43), Ankreuzfeld in Kalenderfarbe. */
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
                              "button.%1$s { background-color: color-mix(in srgb, %2$s 25%%, var(--view-bg-color)); box-shadow: inset 4px 0 %2$s; }\n"
                              "button.%1$s:hover { background-color: color-mix(in srgb, %2$s 35%%, var(--view-bg-color)); }\n",
                              classes[0], color);
    }

  self->colors = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (self->colors, css->str);
  gtk_style_context_add_provider_for_display (gtk_widget_get_display (GTK_WIDGET (self)),
                                              GTK_STYLE_PROVIDER (self->colors),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
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
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, views);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, calendar_model);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, month_weeks);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_header);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_grid);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, hour_bar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, week_scroller);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_popover);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_summary);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_when);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_notes);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_location_box);
  gtk_widget_class_bind_template_child (widget_class, RevolutionCalendarView, event_location);
}

static void
revolution_calendar_view_init (RevolutionCalendarView *self)
{
  g_autoptr (GListModel) calendars = revolution_calendar_sample_calendars ();
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) view_action = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_no_selection_set_model (self->calendar_model, calendars);
  load_colors (self);
  fill_month (self);
  fill_week (self);
  g_signal_connect (gtk_scrolled_window_get_vadjustment (self->week_scroller), "changed",
                    G_CALLBACK (on_week_adjustment_changed), NULL);

  /* calendar.view für Alt+1/Alt+2 wie win.view in main.c. */
  view_action = g_property_action_new ("view", self->views, "visible-child-name");
  g_action_map_add_action_entries (G_ACTION_MAP (group), calendar_actions, G_N_ELEMENTS (calendar_actions), NULL);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (view_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "calendar", G_ACTION_GROUP (group));
}
