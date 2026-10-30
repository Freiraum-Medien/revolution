/* gcal-attendees-section.c – Teilnehmer im Termin-Editor: anzeigen mit Status, hinzufügen, entfernen (#377)
 *
 * Eigener Code. gnome-calendar 50.0 zeigt Teilnehmer nur an (gcal-attendees-section.c:87-97 „read-only for now“,
 * Messung Claude/m7-377-messung.md). Verhalten wie Evolution 3.56.2 (Vorlage, nicht übernommen):
 *   Statustexte                       src/calendar/gui/e-meeting-store.c:190-208 (partstat_to_text)
 *   Organisator = Adresse des Kalenders (cal-email-address), sonst Wahl unter den Identitäten
 *                                     src/calendar/gui/e-comp-editor-page-general.c:530-603
 *   neuer Teilnehmer: REQ-PARTICIPANT, NEEDS-ACTION, RSVP   src/calendar/gui/e-meeting-attendee.c (Vorgaben)
 *   Bearbeiten nur als Organisator   src/calendar/gui/itip-utils.c:227-280 (itip_organizer_is_user)
 * Adressvorschläge aus #361 (src/composer/contact-completion.c). Ohne Rollen, RSVP-Wahl, Delegation, Ressourcen,
 * Verfügbarkeit.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "gcal-attendees-section.h"
#include "gcal-context.h"
#include "gcal-event-editor-section.h"
#include "gcal-utils.h"

#include <string.h>

#include <camel/camel.h>
#include <glib/gi18n.h>
#include <libecal/libecal.h>

#include "src/composer/contact-completion.h"

struct _GcalAttendeesSection
{
  AdwBin              parent;

  GtkWidget          *organizer_list;
  AdwComboRow        *organizer_row;
  GtkWidget          *attendees_list;
  GtkWidget          *add_box;
  GtkWidget          *add_entry;

  GcalContext        *context;
  GcalEvent          *event;
  GPtrArray          *attendees;     /* ECalComponentAttendee */
  GPtrArray          *identities;    /* ESource (Mailidentität), Reihenfolge wie organizer_row */
  char               *organizer;     /* Adresse des gespeicherten Organisators */
  char               *cal_address;   /* cal-email-address des Kalenders */
  gboolean            editable;
  gboolean            changed;
  RevolutionContactCompletion *completion;
};

static void gcal_event_editor_section_iface_init (GcalEventEditorSectionInterface *iface);

G_DEFINE_TYPE_WITH_CODE (GcalAttendeesSection, gcal_attendees_section, ADW_TYPE_BIN,
                         G_IMPLEMENT_INTERFACE (GCAL_TYPE_EVENT_EDITOR_SECTION, gcal_event_editor_section_iface_init))

enum
{
  PROP_0,
  PROP_CONTEXT,
  N_PROPS
};

static ESourceRegistry *
registry (void)
{
  /* main.c legt die Registry der App an der Anwendung ab (wie revolution_composer_new_mailto). */
  return g_object_get_data (G_OBJECT (g_application_get_default ()), "registry");
}

static const char *
identity_address (ESource *identity)
{
  return e_source_mail_identity_get_address (e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY));
}

static gboolean
address_is_user (GcalAttendeesSection *self,
                 const char           *address)
{
  if (address == NULL)
    return FALSE;
  if (self->cal_address != NULL && e_cal_util_email_addresses_equal (self->cal_address, address))
    return TRUE;
  for (guint i = 0; i < self->identities->len; i++)
    if (e_cal_util_email_addresses_equal (identity_address (self->identities->pdata[i]), address))
      return TRUE;
  return FALSE;
}

/* Übernommen aus Evolution 3.56.2 src/calendar/gui/e-meeting-store.c:190-208 (partstat_to_text, Texte). */
static const char *
partstat_text (ICalParameterPartstat partstat)
{
  switch (partstat)
    {
    case I_CAL_PARTSTAT_ACCEPTED:
      return _("Accepted");
    case I_CAL_PARTSTAT_DECLINED:
      return _("Declined");
    case I_CAL_PARTSTAT_TENTATIVE:
      return _("Tentatively Accepted");
    case I_CAL_PARTSTAT_DELEGATED:
      return _("Delegated");
    case I_CAL_PARTSTAT_NEEDSACTION:
    default:
      return _("Needs Action");
    }
}

static void on_remove_clicked_cb (GtkButton *button, GcalAttendeesSection *self);

static void
rebuild_rows (GcalAttendeesSection *self)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->attendees_list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->attendees_list), child);

  if (self->organizer != NULL && self->attendees->len > 0)
    {
      GtkWidget *row = adw_action_row_new ();

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), self->organizer);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _("Organizer"));
      gtk_list_box_append (GTK_LIST_BOX (self->attendees_list), row);
    }

  for (guint i = 0; i < self->attendees->len; i++)
    {
      ECalComponentAttendee *attendee = self->attendees->pdata[i];
      const char *address = e_cal_util_get_attendee_email (attendee);
      const char *name = e_cal_component_attendee_get_cn (attendee);
      const char *status = partstat_text (e_cal_component_attendee_get_partstat (attendee));
      g_autofree char *subtitle = NULL;
      GtkWidget *row = adw_action_row_new ();

      if (name == NULL || *name == '\0')
        name = address;
      subtitle = g_strcmp0 (name, address) != 0 ? g_strdup_printf ("%s · %s", address, status) : g_strdup (status);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name != NULL ? name : "");
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
      if (self->editable)
        {
          GtkWidget *button = gtk_button_new_from_icon_name ("user-trash-symbolic");
          g_autofree char *label = g_strdup_printf (_("Remove %s"), address != NULL ? address : "");

          gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
          gtk_widget_add_css_class (button, "flat");
          gtk_widget_set_tooltip_text (button, _("Remove Attendee"));
          gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
          g_object_set_data (G_OBJECT (button), "attendee", attendee);
          g_signal_connect (button, "clicked", G_CALLBACK (on_remove_clicked_cb), self);
          adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
        }
      gtk_list_box_append (GTK_LIST_BOX (self->attendees_list), row);
    }

  gtk_widget_set_visible (self->attendees_list, gtk_widget_get_first_child (self->attendees_list) != NULL);
  gtk_widget_set_visible (self->add_box, self->editable);
  /* Organisator-Wahl nur ohne gespeicherten Organisator und ohne Adresse des Kalenders (lokal). */
  gtk_widget_set_visible (self->organizer_list, self->editable && self->organizer == NULL && self->cal_address == NULL &&
                                                self->attendees->len > 0 && self->identities->len > 0);
}

static void
on_remove_clicked_cb (GtkButton            *button,
                      GcalAttendeesSection *self)
{
  g_ptr_array_remove (self->attendees, g_object_get_data (G_OBJECT (button), "attendee"));
  self->changed = TRUE;
  rebuild_rows (self);
  gtk_widget_grab_focus (self->add_entry);
}

/* Eingabe „Name <Adresse>, …“ (auch aus den Vorschlägen) wie die Empfängerfelder des Composers zerlegen. */
static void
on_add_cb (GtkWidget            *widget G_GNUC_UNUSED,
           GcalAttendeesSection *self)
{
  g_autoptr (CamelInternetAddress) addresses = camel_internet_address_new ();
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->add_entry));
  gboolean added = FALSE;

  if (camel_address_decode (CAMEL_ADDRESS (addresses), text) <= 0)
    return;

  for (int i = 0; i < camel_address_length (CAMEL_ADDRESS (addresses)); i++)
    {
      const char *name = NULL, *address = NULL;
      g_autofree char *value = NULL;
      gboolean known = FALSE;

      if (!camel_internet_address_get (addresses, i, &name, &address) || address == NULL || strchr (address, '@') == NULL)
        continue;
      for (guint j = 0; j < self->attendees->len && !known; j++)
        known = e_cal_util_email_addresses_equal (e_cal_util_get_attendee_email (self->attendees->pdata[j]), address);
      if (known || (self->organizer != NULL && e_cal_util_email_addresses_equal (self->organizer, address)))
        continue;
      value = g_strconcat ("mailto:", address, NULL);
      g_ptr_array_add (self->attendees,
                       e_cal_component_attendee_new_full (value, NULL, I_CAL_CUTYPE_INDIVIDUAL, I_CAL_ROLE_REQPARTICIPANT,
                                                          I_CAL_PARTSTAT_NEEDSACTION, TRUE, NULL, NULL, NULL,
                                                          name != NULL && *name != '\0' ? name : NULL, NULL));
      added = TRUE;
    }

  if (!added)
    return;
  gtk_editable_set_text (GTK_EDITABLE (self->add_entry), "");
  self->changed = TRUE;
  rebuild_rows (self);
  g_debug ("Kalender: Teilnehmer %u", self->attendees->len);
}

static void
focus_entry_cb (GtkWidget  *widget,
                const char *action_name G_GNUC_UNUSED,
                GVariant   *parameter G_GNUC_UNUSED)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (widget);

  if (self->editable)
    gtk_widget_grab_focus (self->add_entry);
}

/* Kalender des Termins (auch nach Wechsel in der Kalenderwahl des Editors, gcal-event-editor-dialog.c). */
void
gcal_attendees_section_set_calendar (GcalAttendeesSection *self,
                                     GcalCalendar         *calendar)
{
  g_autofree char *address = NULL;

  g_clear_pointer (&self->cal_address, g_free);
  /* Eigenschaft aus dem D-Bus-Proxy, ohne Rundreise (e-cal-client.c:1037-1040). */
  if (calendar != NULL &&
      e_client_get_backend_property_sync (E_CLIENT (gcal_calendar_get_client (calendar)),
                                          E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS, &address, NULL, NULL) &&
      address != NULL && *address != '\0')
    self->cal_address = g_steal_pointer (&address);
  self->editable = self->organizer == NULL || address_is_user (self, self->organizer);
  rebuild_rows (self);
}

static void
gcal_attendees_section_set_event (GcalEventEditorSection *section,
                                  GcalEvent              *event,
                                  GcalEventEditorFlags    flags G_GNUC_UNUSED)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (section);
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  g_autolist (ESource) identities = NULL;
  g_autoptr (ESource) preferred = NULL;
  ECalComponentOrganizer *organizer;
  ECalComponent *component;
  GSList *attendees;
  guint selected = 0;

  g_set_object (&self->event, event);
  g_ptr_array_set_size (self->attendees, 0);
  g_ptr_array_set_size (self->identities, 0);
  g_clear_pointer (&self->organizer, g_free);
  self->changed = FALSE;
  gtk_editable_set_text (GTK_EDITABLE (self->add_entry), "");

  if (registry () != NULL)
    {
      identities = e_source_registry_list_enabled (registry (), E_SOURCE_EXTENSION_MAIL_IDENTITY);
      preferred = e_source_registry_ref_default_mail_identity (registry ());
    }
  for (GList *l = identities; l != NULL; l = l->next)
    {
      const char *name = e_source_mail_identity_get_name (e_source_get_extension (l->data, E_SOURCE_EXTENSION_MAIL_IDENTITY));
      g_autofree char *text = NULL;

      if (identity_address (l->data) == NULL)
        continue;
      if (preferred != NULL && e_source_equal (l->data, preferred))
        selected = self->identities->len;
      text = name != NULL && *name != '\0' ? g_strdup_printf ("%s <%s>", name, identity_address (l->data))
                                           : g_strdup (identity_address (l->data));
      gtk_string_list_append (names, text);
      g_ptr_array_add (self->identities, g_object_ref (l->data));
    }
  adw_combo_row_set_model (self->organizer_row, G_LIST_MODEL (names));
  adw_combo_row_set_selected (self->organizer_row, selected);

  if (event == NULL)
    {
      rebuild_rows (self);
      return;
    }

  component = gcal_event_get_component (event);
  organizer = e_cal_component_get_organizer (component);
  if (organizer != NULL && e_cal_util_get_organizer_email (organizer) != NULL)
    self->organizer = g_strdup (e_cal_util_get_organizer_email (organizer));
  g_clear_pointer (&organizer, e_cal_component_organizer_free);

  attendees = e_cal_component_get_attendees (component);
  for (GSList *l = attendees; l != NULL; l = l->next)
    g_ptr_array_add (self->attendees, l->data);
  g_slist_free (attendees);

  gcal_attendees_section_set_calendar (self, gcal_event_get_calendar (event));
}

static void
gcal_attendees_section_apply (GcalEventEditorSection *section)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (section);
  ECalComponent *component = gcal_event_get_component (self->event);
  g_autoptr (GSList) list = NULL;

  /* Getippte, noch nicht übernommene Adresse beim Speichern übernehmen. */
  if (*gtk_editable_get_text (GTK_EDITABLE (self->add_entry)) != '\0')
    on_add_cb (NULL, self);
  if (!self->changed)
    return;

  for (guint i = self->attendees->len; i > 0; i--)
    list = g_slist_prepend (list, self->attendees->pdata[i - 1]);
  e_cal_component_set_attendees (component, list);

  if (self->attendees->len == 0)
    {
      /* Ohne Teilnehmer kein Termin mit Einladung mehr (Organisator wie bei einem einfachen Termin weg). */
      e_cal_component_set_organizer (component, NULL);
      g_clear_pointer (&self->organizer, g_free);
    }
  else if (self->organizer == NULL)
    {
      guint i = adw_combo_row_get_selected (self->organizer_row);
      const char *address = self->cal_address;
      const char *name = NULL;
      g_autofree char *value = NULL;
      ECalComponentOrganizer *organizer;

      if (address == NULL && i < self->identities->len)
        address = identity_address (self->identities->pdata[i]);
      for (guint j = 0; address != NULL && j < self->identities->len && name == NULL; j++)
        if (e_cal_util_email_addresses_equal (identity_address (self->identities->pdata[j]), address))
          name = e_source_mail_identity_get_name (e_source_get_extension (self->identities->pdata[j],
                                                                          E_SOURCE_EXTENSION_MAIL_IDENTITY));
      if (address == NULL)
        return;
      value = g_strconcat ("mailto:", address, NULL);
      organizer = e_cal_component_organizer_new_full (value, NULL, name, NULL);
      e_cal_component_set_organizer (component, organizer);
      e_cal_component_organizer_free (organizer);
      self->organizer = g_strdup (address);
    }
  self->changed = FALSE;
}

static gboolean
gcal_attendees_section_changed (GcalEventEditorSection *section)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (section);

  return self->changed || (self->editable && *gtk_editable_get_text (GTK_EDITABLE (self->add_entry)) != '\0');
}

static void
gcal_event_editor_section_iface_init (GcalEventEditorSectionInterface *iface)
{
  iface->set_event = gcal_attendees_section_set_event;
  iface->apply = gcal_attendees_section_apply;
  iface->changed = gcal_attendees_section_changed;
}

static void
gcal_attendees_section_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, self->context);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_attendees_section_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_set_object (&self->context, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_attendees_section_dispose (GObject *object)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  if (self->completion != NULL)
    g_object_run_dispose (G_OBJECT (self->completion));
  g_clear_object (&self->completion);

  G_OBJECT_CLASS (gcal_attendees_section_parent_class)->dispose (object);
}

static void
gcal_attendees_section_finalize (GObject *object)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  g_clear_object (&self->context);
  g_clear_object (&self->event);
  g_clear_pointer (&self->attendees, g_ptr_array_unref);
  g_clear_pointer (&self->identities, g_ptr_array_unref);
  g_free (self->organizer);
  g_free (self->cal_address);

  G_OBJECT_CLASS (gcal_attendees_section_parent_class)->finalize (object);
}

static void
gcal_attendees_section_class_init (GcalAttendeesSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gcal_attendees_section_dispose;
  object_class->finalize = gcal_attendees_section_finalize;
  object_class->get_property = gcal_attendees_section_get_property;
  object_class->set_property = gcal_attendees_section_set_property;

  g_object_class_install_property (object_class, PROP_CONTEXT,
                                   g_param_spec_object ("context", NULL, NULL, GCAL_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/event-editor/gcal-attendees-section.ui");
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, organizer_list);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, organizer_row);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, attendees_list);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, add_box);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, add_entry);
  gtk_widget_class_bind_template_callback (widget_class, on_add_cb);

  /* Alt+I springt in das Feld „Teilnehmer hinzufügen“ (Kürzel in gcal-event-editor-dialog.blp). */
  gtk_widget_class_install_action (widget_class, "attendees.focus", NULL, focus_entry_cb);
}

static void
gcal_attendees_section_init (GcalAttendeesSection *self)
{
  self->attendees = g_ptr_array_new_with_free_func ((GDestroyNotify) e_cal_component_attendee_free);
  self->identities = g_ptr_array_new_with_free_func (g_object_unref);

  gtk_widget_init_template (GTK_WIDGET (self));

  if (registry () != NULL)
    {
      self->completion = revolution_contact_completion_new (registry ());
      revolution_contact_completion_add_entry (self->completion, GTK_ENTRY (self->add_entry));
    }
}
