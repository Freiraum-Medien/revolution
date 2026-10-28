/* contacts-detail.c – Detailansicht eines Kontakts (#369)
 *
 * Zeigt den EContact des gewählten Listeneintrags (RevolutionContactsContact aus der EBookClientView in
 * contacts-view.c, #368); Oberfläche in contacts-detail.blp. Vorlage: quelltext/stamp/src/views/contact/
 * stamp-contact-details.c:150-377 (je Feldtyp eine Liste aus AdwActionRow, leere Listen verborgen).
 * Felder und Reihenfolge nach Evolution 3.56.2 eab-contact-formatter.c: alle E-Mails mit Ort aus TYPE
 * (get_email_location, 512-526: WORK/HOME/OTHER), alle Telefone aus E_CONTACT_TEL (accum_tel, 792-855),
 * Adressen Arbeit/Privat/Weitere (accum_address, 160-256: zuerst das fertige Adress-Label, sonst die Teile
 * zeilenweise), Firma/Abteilung/Position (render_work_column, 998-1002), Notiz (render_footer, 1093-1116).
 * Abweichungen: Telefon-Typ nur Mobil/Fax/Pager/Arbeit/Privat (Evolution trennt Arbeit/Privat in Spalten);
 * Postleitzahl und Ort in einer Zeile wie Stamp (stamp-contact-details.c:325-331) statt je einer Zeile.
 * Foto: das Foto des Listeneintrags (contacts-contact.c lädt es im Worker-GTask), sonst Initialen.
 * Keine EDS-Aufrufe hier: der EContact liegt schon im Speicher.
 *
 * „Mail schreiben“ öffnet den Composer über denselben Weg wie ein mailto-Link (main.c open_mailto,
 * revolution_composer_new_mailto) mit „Name <Adresse>“ in An; wie Evolutions „Nachricht an Kontakt
 * schicken“ (e-book-shell-view-actions.c:1469-1474) ohne Tastenkürzel.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>
#include <libebook/libebook.h>

#include "composer/composer.h"
#include "contacts-detail.h"

struct _RevolutionContactsDetail
{
  AdwBin                     parent_instance;
  GtkStack                  *stack;
  AdwAvatar                 *avatar;
  GtkLabel                  *name_label;
  GtkLabel                  *org_label;
  GtkWidget                 *mail_button;
  GtkListBox                *email_list;
  GtkListBox                *phone_list;
  GtkListBox                *address_list;
  GtkListBox                *org_list;
  GtkListBox                *note_list;

  RevolutionContactsContact *item;
  GBinding                  *photo_binding;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsDetail, revolution_contacts_detail, ADW_TYPE_BIN)

/* Zeile einer Karte: Symbol klein und grau, Typ gedimmt oben, Wert darunter (Stilklasse property),
 * Wert auswählbar zum Kopieren; ohne Markup, Werte kommen roh aus der vCard. */
static GtkWidget *
add_row (GtkListBox *list,
         const char *icon_name,
         const char *label,
         const char *value)
{
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *icon = g_object_new (GTK_TYPE_IMAGE, "icon-name", icon_name,
                                  "accessible-role", GTK_ACCESSIBLE_ROLE_PRESENTATION, NULL);

  gtk_widget_add_css_class (icon, "dimmed");
  gtk_widget_add_css_class (row, "property");
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), label);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
  gtk_list_box_append (list, row);
  gtk_widget_set_visible (GTK_WIDGET (list), TRUE);
  return row;
}

/* Empfänger „Name <Adresse>“ wie Camel ihn schreibt (Anführungszeichen, wenn nötig). */
static char *
recipient (EContact   *contact,
           const char *address)
{
  g_autoptr (CamelInternetAddress) cia = camel_internet_address_new ();

  camel_internet_address_add (cia, e_contact_get_const (contact, E_CONTACT_FULL_NAME), address);
  return camel_address_format (CAMEL_ADDRESS (cia));
}

/* Ort einer E-Mail-Adresse aus TYPE wie Evolution get_email_location (eab-contact-formatter.c:512-526): WORK,
 * HOME; OTHER (so liefert microsoft365 jede Adresse, e-book-backend-m365.c:866-867) und ohne Ort nur „E-Mail“. */
static const char *
email_label (EVCardAttribute *attr)
{
  if (e_vcard_attribute_has_type (attr, "WORK"))
    return _("Work");
  if (e_vcard_attribute_has_type (attr, "HOME"))
    return _("Home");
  return _("Email");
}

static void
show_emails (RevolutionContactsDetail *self,
             EContact                 *contact)
{
  GList *attrs = e_contact_get_attributes (contact, E_CONTACT_EMAIL);
  gboolean first = TRUE;

  for (GList *l = attrs; l != NULL; l = l->next)
    {
      g_autofree char *address = e_vcard_attribute_get_value (l->data);
      g_autofree char *to = NULL;
      g_autofree char *tooltip = NULL;
      GtkWidget *row, *button;

      if (address == NULL || *g_strstrip (address) == '\0')
        continue;
      to = recipient (contact, address);
      row = add_row (self->email_list, "mail-unread-symbolic", email_label (l->data), address);
      /* Aktion je Zeile: Papierflieger (Dokument #5, Prototyp sent-symbolic). */
      tooltip = g_strdup_printf (_("Send Email to %s"), address);
      button = gtk_button_new_from_icon_name ("sent-symbolic");
      gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (button, tooltip);
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
      gtk_widget_add_css_class (button, "flat");
      gtk_widget_add_css_class (button, "circular");
      gtk_actionable_set_action_target (GTK_ACTIONABLE (button), "s", to);
      gtk_actionable_set_action_name (GTK_ACTIONABLE (button), "detail.write-mail");
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
      /* Ziel vor dem Namen, sonst meldet GtkActionHelper ein fehlendes Ziel. */
      if (first)
        {
          gtk_actionable_set_action_target (GTK_ACTIONABLE (self->mail_button), "s", to);
          gtk_actionable_set_action_name (GTK_ACTIONABLE (self->mail_button), "detail.write-mail");
        }
      first = FALSE;
    }
  gtk_widget_set_visible (self->mail_button, !first);
  g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);
}

/* Typ einer Telefonnummer aus TYPE (vCard 3.0: CELL, FAX, PAGER, WORK, HOME, VOICE …). */
static const char *
phone_label (EVCardAttribute *attr)
{
  if (e_vcard_attribute_has_type (attr, "CELL"))
    return _("Mobile");
  if (e_vcard_attribute_has_type (attr, "FAX"))
    return _("Fax");
  if (e_vcard_attribute_has_type (attr, "PAGER"))
    return _("Pager");
  if (e_vcard_attribute_has_type (attr, "WORK"))
    return _("Work");
  if (e_vcard_attribute_has_type (attr, "HOME"))
    return _("Home");
  return _("Phone");
}

/* Nur Anzeige (auswählbar); keine Anruf-Aktion in M6 (Dokument #19 „Nicht in M6“). */
static void
show_phones (RevolutionContactsDetail *self,
             EContact                 *contact)
{
  GList *attrs = e_contact_get_attributes (contact, E_CONTACT_TEL);

  for (GList *l = attrs; l != NULL; l = l->next)
    {
      g_autofree char *number = e_vcard_attribute_get_value (l->data);

      if (number != NULL && *g_strstrip (number) != '\0')
        add_row (self->phone_list, "phone-symbolic", phone_label (l->data), number);
    }
  g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);
}

static void
append_line (GString    *text,
             const char *line)
{
  if (line == NULL || *line == '\0')
    return;
  if (text->len > 0)
    g_string_append_c (text, '\n');
  g_string_append (text, line);
}

/* Wie Evolution accum_address (eab-contact-formatter.c:160-256): fertiges Label der vCard (LABEL), sonst
 * Postfach, Zusatz, Straße, PLZ Ort, Region, Land je in einer Zeile. */
static void
show_address (RevolutionContactsDetail *self,
              EContact                 *contact,
              EContactField             address_field,
              EContactField             label_field,
              const char               *label)
{
  const char *formatted = e_contact_get_const (contact, label_field);
  g_autoptr (EContactAddress) address = NULL;
  g_autoptr (GString) text = NULL;
  g_autofree char *place = NULL;

  if (formatted != NULL && *formatted != '\0')
    {
      add_row (self->address_list, "mark-location-symbolic", label, formatted);
      return;
    }
  address = e_contact_get (contact, address_field);
  if (address == NULL)
    return;
  text = g_string_new (NULL);
  append_line (text, address->po);
  append_line (text, address->ext);
  append_line (text, address->street);
  place = g_strstrip (g_strjoin (" ", address->code != NULL ? address->code : "",
                                 address->locality != NULL ? address->locality : "", NULL));
  append_line (text, place);
  append_line (text, address->region);
  append_line (text, address->country);
  if (text->len > 0)
    add_row (self->address_list, "mark-location-symbolic", label, text->str);
}

static void
show_field (GtkListBox  *list,
            EContact    *contact,
            EContactField field,
            const char  *icon_name,
            const char  *label)
{
  const char *value = e_contact_get_const (contact, field);

  if (value != NULL && *value != '\0')
    add_row (list, icon_name, label, value);
}

static void
clear_list (GtkListBox *list)
{
  gtk_list_box_remove_all (list);
  gtk_widget_set_visible (GTK_WIDGET (list), FALSE);
}

static guint
count_rows (GtkListBox *list)
{
  guint n = 0;

  while (gtk_list_box_get_row_at_index (list, (int) n) != NULL)
    n++;
  return n;
}

static void
show_contact (RevolutionContactsDetail *self)
{
  EContact *contact = revolution_contacts_contact_get_contact (self->item);
  const char *title = e_contact_get_const (contact, E_CONTACT_TITLE);
  const char *org = e_contact_get_const (contact, E_CONTACT_ORG);
  g_autofree char *subtitle = NULL;
  GtkListBox *lists[] = { self->email_list, self->phone_list, self->address_list, self->org_list, self->note_list };

  for (gsize i = 0; i < G_N_ELEMENTS (lists); i++)
    clear_list (lists[i]);

  adw_avatar_set_text (self->avatar, revolution_contacts_contact_get_name (self->item));
  gtk_label_set_label (self->name_label, revolution_contacts_contact_get_name (self->item));
  /* Unter dem Namen Position · Firma (Dokument #19 #369). */
  if (title != NULL && *title != '\0' && org != NULL && *org != '\0')
    subtitle = g_strdup_printf ("%s · %s", title, org);
  else
    subtitle = g_strdup (title != NULL && *title != '\0' ? title : org);
  gtk_label_set_label (self->org_label, subtitle);
  gtk_widget_set_visible (GTK_WIDGET (self->org_label), subtitle != NULL && *subtitle != '\0');

  show_emails (self, contact);
  show_phones (self, contact);
  show_address (self, contact, E_CONTACT_ADDRESS_WORK, E_CONTACT_ADDRESS_LABEL_WORK, _("Work"));
  show_address (self, contact, E_CONTACT_ADDRESS_HOME, E_CONTACT_ADDRESS_LABEL_HOME, _("Home"));
  show_address (self, contact, E_CONTACT_ADDRESS_OTHER, E_CONTACT_ADDRESS_LABEL_OTHER, _("Other"));
  show_field (self->org_list, contact, E_CONTACT_ORG, "building-symbolic", _("Company"));
  show_field (self->org_list, contact, E_CONTACT_ORG_UNIT, "building-symbolic", _("Department"));
  show_field (self->org_list, contact, E_CONTACT_TITLE, "building-symbolic", _("Position"));
  show_field (self->note_list, contact, E_CONTACT_NOTE, "text-x-generic-symbolic", _("Notes"));

  g_debug ("Kontakt-Detail: %s „%s“, E-Mail %u, Telefon %u, Adressen %u, Organisation %u, Notiz %u, Foto %s",
           revolution_contacts_contact_get_uid (self->item), revolution_contacts_contact_get_name (self->item),
           count_rows (self->email_list), count_rows (self->phone_list), count_rows (self->address_list),
           count_rows (self->org_list), count_rows (self->note_list),
           e_vcard_get_attribute (E_VCARD (contact), EVC_PHOTO) != NULL ? "ja" : "nein");
}

/* Neuer Eintrag, derselbe mit neuen Daten (objects-modified, contacts-view.c) oder NULL (leer). */
void
revolution_contacts_detail_set_contact (RevolutionContactsDetail  *self,
                                        RevolutionContactsContact *item)
{
  g_return_if_fail (REVOLUTION_IS_CONTACTS_DETAIL (self));

  if (item != self->item)
    {
      g_clear_pointer (&self->photo_binding, g_binding_unbind);
      g_set_object (&self->item, item);
      /* Liest „photo“ des Eintrags (lädt es im Worker, falls die Listenzeile es noch nicht tat). */
      if (item != NULL)
        self->photo_binding = g_object_bind_property (item, "photo", self->avatar, "custom-image", G_BINDING_SYNC_CREATE);
    }
  if (item == NULL)
    {
      g_debug ("Kontakt-Detail: leer");
      gtk_stack_set_visible_child_name (self->stack, "empty");
      return;
    }
  show_contact (self);
  gtk_stack_set_visible_child_name (self->stack, "contact");
}

/* detail.write-mail: Composer wie ein mailto-Link (main.c open_mailto), nur geöffnet, nicht gesendet. */
static void
on_write_mail (GSimpleAction *action G_GNUC_UNUSED,
               GVariant      *parameter,
               gpointer       user_data)
{
  RevolutionContactsDetail *self = user_data;
  GtkApplication *application = gtk_window_get_application (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))));
  g_autofree char *escaped = g_uri_escape_string (g_variant_get_string (parameter, NULL), "@", FALSE);
  g_autofree char *mailto = g_strconcat ("mailto:", escaped, NULL);

  g_debug ("Kontakt-Detail: Mail an %s", g_variant_get_string (parameter, NULL));
  gtk_window_present (revolution_composer_new_mailto (application, g_object_get_data (G_OBJECT (application), "registry"),
                                                      g_object_get_data (G_OBJECT (application), "session"), mailto));
}

static const GActionEntry detail_actions[] = {
  { "write-mail", on_write_mail, "s", NULL, NULL, { 0 } },
};

static void
revolution_contacts_detail_dispose (GObject *object)
{
  RevolutionContactsDetail *self = REVOLUTION_CONTACTS_DETAIL (object);

  g_clear_pointer (&self->photo_binding, g_binding_unbind);
  g_clear_object (&self->item);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_CONTACTS_DETAIL);

  G_OBJECT_CLASS (revolution_contacts_detail_parent_class)->dispose (object);
}

static void
revolution_contacts_detail_class_init (RevolutionContactsDetailClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_contacts_detail_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/contacts-detail.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, avatar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, name_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, org_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, mail_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, email_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, phone_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, address_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, org_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsDetail, note_list);
}

static void
revolution_contacts_detail_init (RevolutionContactsDetail *self)
{
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();

  gtk_widget_init_template (GTK_WIDGET (self));
  g_action_map_add_action_entries (G_ACTION_MAP (group), detail_actions, G_N_ELEMENTS (detail_actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "detail", G_ACTION_GROUP (group));
}
