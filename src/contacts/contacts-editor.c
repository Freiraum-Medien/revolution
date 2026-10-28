/* contacts-editor.c – Bearbeitungsformular eines Kontakts (#370)
 *
 * Oberfläche in contacts-editor.blp. Das Formular kennt nur seine Felder; Schreiben, Buchwahl und Fehler
 * übernimmt contacts-view.c. Vorlage für die Feldbelegung: Evolution 3.56.2
 * src/addressbook/gui/contact-editor/e-contact-editor.c – Arbeit am Duplikat des EContact (der Editor hält
 * editor->priv->contact und setzt beim Speichern nur seine Felder, extract_all), Werte, die das Formular nicht
 * kennt (weitere vCard-Attribute, Parameter wie X-EVOLUTION-UI-SLOT, Foto), bleiben so erhalten. Typen:
 * E-Mail Arbeit/Privat/Weitere (eab-book-util.c:31-36 email_types), Telefon nach eab_phone_types
 * (eab-book-util.c:47-65) auf die Typen der Detailansicht (#369) beschränkt, Erkennung wie
 * eab_get_phone_type_index (eab-book-util.c:236-249). TYPE einer bestehenden Zeile ändert sich nur, wenn die
 * Auswahl geändert wurde. Adresse: Felder Straße, PLZ, Ort, Region, Land; Postfach und Zusatz bleiben aus dem
 * Original erhalten; geänderte Adressen bekommen ein neues Label wie set_address_label (e-contact-editor.c:
 * 2412-2469, ohne die Länderformatierung eab_format_address), sonst zeigte die Detailansicht das alte Label.
 * Ablegen unter (FILE_AS): Stil „Nachname, Vorname“ wie Evolutions Vorgabe (name_to_style Stil 0); microsoft365
 * verlangt das Feld (tests/testdaten.py, #368).
 * Gültig wie Evolutions „OK“ inaktiv ohne Namen (sensitize_ok, e-contact-editor.c:758-790), hier nach Vorgabe
 * #370: Name oder E-Mail-Adresse.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "contacts-editor.h"

typedef struct
{
  const char *label;
  const char *type_1;
  const char *type_2;
} TypeEntry;

static const TypeEntry email_types[] = {
  { N_("Work"), "WORK", NULL },
  { N_("Home"), "HOME", NULL },
  { N_("Other"), "OTHER", NULL },
};

static const TypeEntry phone_types[] = {
  { N_("Mobile"), "CELL", NULL },
  { N_("Work"), "WORK", "VOICE" },
  { N_("Home"), "HOME", "VOICE" },
  { N_("Work Fax"), "WORK", "FAX" },
  { N_("Home Fax"), "HOME", "FAX" },
  { N_("Pager"), "PAGER", NULL },
  { N_("Other"), "VOICE", NULL },
};

/* Reihenfolge wie die Detailansicht (contacts-detail.c show_address). */
static const struct { const char *label; EContactField address; EContactField label_field; } address_types[] = {
  { N_("Work"), E_CONTACT_ADDRESS_WORK, E_CONTACT_ADDRESS_LABEL_WORK },
  { N_("Home"), E_CONTACT_ADDRESS_HOME, E_CONTACT_ADDRESS_LABEL_HOME },
  { N_("Other"), E_CONTACT_ADDRESS_OTHER, E_CONTACT_ADDRESS_LABEL_OTHER },
};

/* Zeile für E-Mail oder Telefon (Daten an der AdwEntryRow). */
typedef struct
{
  const TypeEntry *types;
  guint            n_types;
  EVCardAttribute *attr;     /* Original, NULL bei neuer Zeile */
  guint            initial;  /* Typ beim Öffnen */
  GtkDropDown     *type;
} ValueRow;

/* Zeile einer Adresse (Daten an der AdwExpanderRow). */
typedef struct
{
  EContactAddress *original; /* Postfach, Zusatz bleiben erhalten */
  GtkDropDown     *type;
  GtkEditable     *street, *code, *locality, *region, *country;
} AddressRow;

struct _RevolutionContactsEditor
{
  AdwBin                parent_instance;
  GtkScrolledWindow    *scroller;
  AdwPreferencesGroup  *book_group;
  AdwComboRow          *book_row;
  AdwEntryRow          *given_row;
  AdwEntryRow          *family_row;
  AdwPreferencesGroup  *email_group;
  AdwPreferencesGroup  *phone_group;
  AdwPreferencesGroup  *address_group;
  GtkWidget            *add_address_button;
  AdwEntryRow          *org_row;
  AdwEntryRow          *unit_row;
  AdwEntryRow          *title_row;
  GtkTextView          *note_view;

  EContact             *original;
  GPtrArray            *email_rows;   /* AdwEntryRow (geliehen, im Formular) */
  GPtrArray            *phone_rows;
  GPtrArray            *address_rows; /* AdwExpanderRow */
  gboolean              valid;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsEditor, revolution_contacts_editor, ADW_TYPE_BIN)

enum { PROP_0, PROP_VALID, N_PROPS };

static GParamSpec *props[N_PROPS];

static void
value_row_free (ValueRow *data)
{
  g_clear_pointer (&data->attr, e_vcard_attribute_free);
  g_free (data);
}

static void
address_row_free (AddressRow *data)
{
  g_clear_pointer (&data->original, e_contact_address_free);
  g_free (data);
}

static const char *
text_of (GtkEditable *editable)
{
  return gtk_editable_get_text (editable);
}

static gboolean
is_blank (const char *text)
{
  if (text == NULL)
    return TRUE;
  while (g_ascii_isspace (*text))
    text++;
  return *text == '\0';
}

static gboolean
has_duplicate_address_type (RevolutionContactsEditor *self)
{
  guint used = 0;

  for (guint i = 0; i < self->address_rows->len; i++)
    {
      AddressRow *data = g_object_get_data (g_ptr_array_index (self->address_rows, i), "address");
      guint bit = 1u << gtk_drop_down_get_selected (data->type);

      if (used & bit)
        return TRUE;
      used |= bit;
    }
  return FALSE;
}

static void
update_valid (RevolutionContactsEditor *self)
{
  gboolean valid = !is_blank (text_of (GTK_EDITABLE (self->given_row))) ||
                   !is_blank (text_of (GTK_EDITABLE (self->family_row)));

  for (guint i = 0; !valid && i < self->email_rows->len; i++)
    valid = !is_blank (text_of (g_ptr_array_index (self->email_rows, i)));
  /* Eine Adresse je Typ (vCard: ADR je WORK/HOME/OTHER, EContact-Felder ADDRESS_WORK/_HOME/_OTHER). */
  valid = valid && !has_duplicate_address_type (self);
  if (valid == self->valid)
    return;
  self->valid = valid;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VALID]);
}

static void
on_changed (RevolutionContactsEditor *self)
{
  update_valid (self);
}

static GtkDropDown *
type_drop_down (const char        *a11y_label,
                const char *const *labels,
                guint              selected)
{
  GtkDropDown *drop_down = GTK_DROP_DOWN (gtk_drop_down_new_from_strings (labels));

  gtk_drop_down_set_selected (drop_down, selected);
  gtk_widget_set_valign (GTK_WIDGET (drop_down), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (drop_down), "flat");
  gtk_widget_set_tooltip_text (GTK_WIDGET (drop_down), a11y_label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (drop_down), GTK_ACCESSIBLE_PROPERTY_LABEL, a11y_label, -1);
  return drop_down;
}

static GtkWidget *
remove_button (const char *a11y_label,
               GCallback   callback,
               gpointer    row)
{
  GtkWidget *button = gtk_button_new_from_icon_name ("user-trash-symbolic");

  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_tooltip_text (button, a11y_label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, a11y_label, -1);
  g_signal_connect_swapped (button, "clicked", callback, row);
  return button;
}

static void
on_remove_value (GtkWidget *row)
{
  RevolutionContactsEditor *self = REVOLUTION_CONTACTS_EDITOR (gtk_widget_get_ancestor (row, REVOLUTION_TYPE_CONTACTS_EDITOR));
  ValueRow *data = g_object_get_data (G_OBJECT (row), "value");
  gboolean email = data->types == email_types;

  g_ptr_array_remove (email ? self->email_rows : self->phone_rows, row);
  adw_preferences_group_remove (email ? self->email_group : self->phone_group, row);
  update_valid (self);
}

static guint
type_index (EVCardAttribute *attr,
            const TypeEntry *types,
            guint            n_types)
{
  guint n_params = g_list_length (e_vcard_attribute_get_param (attr, EVC_TYPE));

  for (guint i = 0; i < n_types; i++)
    if (e_vcard_attribute_has_type (attr, types[i].type_1) &&
        (types[i].type_2 == NULL || e_vcard_attribute_has_type (attr, types[i].type_2) ||
         (g_ascii_strcasecmp (types[i].type_2, "VOICE") == 0 && n_params == 1)))
      return i;
  return n_types - 1;
}

/* Zeile anfügen; attr NULL = neue, leere Zeile mit Typ selected. */
static GtkWidget *
add_value_row (RevolutionContactsEditor *self,
               gboolean                  email,
               EVCardAttribute          *attr,
               guint                     selected)
{
  const TypeEntry *types = email ? email_types : phone_types;
  guint n_types = email ? G_N_ELEMENTS (email_types) : G_N_ELEMENTS (phone_types);
  const char *labels[G_N_ELEMENTS (phone_types) + 1] = { NULL };
  GtkWidget *row = adw_entry_row_new ();
  ValueRow *data = g_new0 (ValueRow, 1);
  g_autofree char *value = attr != NULL ? e_vcard_attribute_get_value (attr) : NULL;

  for (guint i = 0; i < n_types; i++)
    labels[i] = _(types[i].label);
  data->types = types;
  data->n_types = n_types;
  data->attr = attr != NULL ? e_vcard_attribute_copy (attr) : NULL;
  data->initial = attr != NULL ? type_index (attr, types, n_types) : selected;
  data->type = type_drop_down (email ? _("Email Type") : _("Phone Type"), labels, data->initial);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), email ? _("Email") : _("Phone"));
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  if (email)
    adw_entry_row_set_input_purpose (ADW_ENTRY_ROW (row), GTK_INPUT_PURPOSE_EMAIL);
  else
    adw_entry_row_set_input_purpose (ADW_ENTRY_ROW (row), GTK_INPUT_PURPOSE_PHONE);
  gtk_editable_set_text (GTK_EDITABLE (row), value != NULL ? value : "");
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (row), GTK_WIDGET (data->type));
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (row), remove_button (email ? _("Remove Email") : _("Remove Phone Number"),
                                                                G_CALLBACK (on_remove_value), row));
  g_object_set_data_full (G_OBJECT (row), "value", data, (GDestroyNotify) value_row_free);
  g_signal_connect_swapped (row, "changed", G_CALLBACK (update_valid), self);

  g_ptr_array_add (email ? self->email_rows : self->phone_rows, row);
  adw_preferences_group_add (email ? self->email_group : self->phone_group, row);
  return row;
}

static void
on_add_email (RevolutionContactsEditor *self)
{
  gtk_widget_grab_focus (add_value_row (self, TRUE, NULL, 0));
}

static void
on_add_phone (RevolutionContactsEditor *self)
{
  gtk_widget_grab_focus (add_value_row (self, FALSE, NULL, 0));
}

static void
on_remove_address (GtkWidget *row)
{
  RevolutionContactsEditor *self = REVOLUTION_CONTACTS_EDITOR (gtk_widget_get_ancestor (row, REVOLUTION_TYPE_CONTACTS_EDITOR));

  g_ptr_array_remove (self->address_rows, row);
  adw_preferences_group_remove (self->address_group, row);
  gtk_widget_set_sensitive (self->add_address_button, self->address_rows->len < G_N_ELEMENTS (address_types));
  update_valid (self);
}

static GtkEditable *
address_entry (GtkWidget  *expander,
               const char *title,
               const char *value)
{
  GtkWidget *row = adw_entry_row_new ();

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  gtk_editable_set_text (GTK_EDITABLE (row), value != NULL ? value : "");
  adw_expander_row_add_row (ADW_EXPANDER_ROW (expander), row);
  return GTK_EDITABLE (row);
}

static GtkWidget *
add_address_row (RevolutionContactsEditor *self,
                 const EContactAddress    *address,
                 guint                     type)
{
  const char *labels[G_N_ELEMENTS (address_types) + 1] = { NULL };
  GtkWidget *row = adw_expander_row_new ();
  AddressRow *data = g_new0 (AddressRow, 1);

  for (guint i = 0; i < G_N_ELEMENTS (address_types); i++)
    labels[i] = _(address_types[i].label);
  data->original = address != NULL ? g_boxed_copy (E_TYPE_CONTACT_ADDRESS, address) : NULL;
  data->type = type_drop_down (_("Address Type"), labels, type);
  g_signal_connect_swapped (data->type, "notify::selected", G_CALLBACK (update_valid), self);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Address"));
  /* AdwExpanderRow reiht Suffixe von rechts nach links: Löschen zuerst, damit Typ | Löschen wie bei E-Mail/Telefon. */
  adw_expander_row_add_suffix (ADW_EXPANDER_ROW (row), remove_button (_("Remove Address"), G_CALLBACK (on_remove_address), row));
  adw_expander_row_add_suffix (ADW_EXPANDER_ROW (row), GTK_WIDGET (data->type));
  data->street = address_entry (row, _("Street"), address != NULL ? address->street : NULL);
  data->code = address_entry (row, _("Postal Code"), address != NULL ? address->code : NULL);
  data->locality = address_entry (row, _("City"), address != NULL ? address->locality : NULL);
  data->region = address_entry (row, _("State/Province"), address != NULL ? address->region : NULL);
  data->country = address_entry (row, _("Country"), address != NULL ? address->country : NULL);
  adw_expander_row_set_expanded (ADW_EXPANDER_ROW (row), TRUE);
  g_object_set_data_full (G_OBJECT (row), "address", data, (GDestroyNotify) address_row_free);

  g_ptr_array_add (self->address_rows, row);
  adw_preferences_group_add (self->address_group, row);
  gtk_widget_set_sensitive (self->add_address_button, self->address_rows->len < G_N_ELEMENTS (address_types));
  return row;
}

/* „+“: erster noch freier Typ. */
static void
on_add_address (RevolutionContactsEditor *self)
{
  guint type = 0;
  AddressRow *data;

  while (type < G_N_ELEMENTS (address_types))
    {
      gboolean used = FALSE;

      for (guint i = 0; i < self->address_rows->len; i++)
        {
          data = g_object_get_data (g_ptr_array_index (self->address_rows, i), "address");
          used = used || gtk_drop_down_get_selected (data->type) == type;
        }
      if (!used)
        break;
      type++;
    }
  if (type == G_N_ELEMENTS (address_types))
    return;
  data = g_object_get_data (G_OBJECT (add_address_row (self, NULL, type)), "address");
  gtk_widget_grab_focus (GTK_WIDGET (data->street));
}

static void
clear_rows (AdwPreferencesGroup *group,
            GPtrArray           *rows)
{
  for (guint i = 0; i < rows->len; i++)
    adw_preferences_group_remove (group, g_ptr_array_index (rows, i));
  g_ptr_array_set_size (rows, 0);
}

static void
set_text (gpointer    editable,
          const char *text)
{
  gtk_editable_set_text (GTK_EDITABLE (editable), text != NULL ? text : "");
}

/* Formular füllen. contact: zu bearbeitender Kontakt oder Vorlage (NULL = leer); books: beim Anlegen die
 * Bezeichnungen der schreibbaren Bücher (GtkStringList, Auswahl selected), beim Bearbeiten NULL. */
void
revolution_contacts_editor_edit (RevolutionContactsEditor *self,
                                 EContact                 *contact,
                                 GListModel               *books,
                                 guint                     selected)
{
  g_autoptr (EContactName) name = NULL;
  g_autoptr (GtkTextBuffer) buffer = NULL;
  GList *attrs;

  g_return_if_fail (REVOLUTION_IS_CONTACTS_EDITOR (self));

  g_clear_object (&self->original);
  self->original = contact != NULL ? e_contact_duplicate (contact) : e_contact_new ();

  gtk_widget_set_visible (GTK_WIDGET (self->book_group), books != NULL);
  adw_combo_row_set_model (self->book_row, books);
  if (books != NULL)
    adw_combo_row_set_selected (self->book_row, selected);

  name = e_contact_get (self->original, E_CONTACT_NAME);
  set_text (self->given_row, name != NULL ? name->given : NULL);
  set_text (self->family_row, name != NULL ? name->family : NULL);

  clear_rows (self->email_group, self->email_rows);
  attrs = e_contact_get_attributes (self->original, E_CONTACT_EMAIL);
  for (GList *l = attrs; l != NULL; l = l->next)
    add_value_row (self, TRUE, l->data, 0);
  g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);
  if (self->email_rows->len == 0)
    add_value_row (self, TRUE, NULL, 0);

  clear_rows (self->phone_group, self->phone_rows);
  attrs = e_contact_get_attributes (self->original, E_CONTACT_TEL);
  for (GList *l = attrs; l != NULL; l = l->next)
    add_value_row (self, FALSE, l->data, 0);
  g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);
  if (self->phone_rows->len == 0)
    add_value_row (self, FALSE, NULL, 0);

  clear_rows (self->address_group, self->address_rows);
  for (guint i = 0; i < G_N_ELEMENTS (address_types); i++)
    {
      g_autoptr (EContactAddress) address = e_contact_get (self->original, address_types[i].address);

      if (address != NULL)
        add_address_row (self, address, i);
    }
  gtk_widget_set_sensitive (self->add_address_button, self->address_rows->len < G_N_ELEMENTS (address_types));

  set_text (self->org_row, e_contact_get_const (self->original, E_CONTACT_ORG));
  set_text (self->unit_row, e_contact_get_const (self->original, E_CONTACT_ORG_UNIT));
  set_text (self->title_row, e_contact_get_const (self->original, E_CONTACT_TITLE));
  buffer = gtk_text_buffer_new (NULL);
  gtk_text_buffer_set_text (buffer, e_contact_get_const (self->original, E_CONTACT_NOTE) != NULL
                            ? e_contact_get_const (self->original, E_CONTACT_NOTE) : "", -1);
  gtk_text_view_set_buffer (self->note_view, buffer);

  self->valid = !self->valid;
  update_valid (self);
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->scroller), 0);
  gtk_widget_grab_focus (GTK_WIDGET (books != NULL ? GTK_WIDGET (self->book_row) : GTK_WIDGET (self->given_row)));
}

static char *
stripped (gpointer editable)
{
  char *text = g_strstrip (g_strdup (text_of (GTK_EDITABLE (editable))));

  if (*text == '\0')
    g_clear_pointer (&text, g_free);
  return text;
}

/* Wie Evolution name_to_style Stil 0 (e-contact-editor.c): „Nachname, Vorname“. */
static char *
file_as_for (const char *given,
             const char *family)
{
  if (given != NULL && *given != '\0' && family != NULL && *family != '\0')
    return g_strdup_printf ("%s, %s", family, given);
  return g_strdup (family != NULL && *family != '\0' ? family : given);
}

static void
build_name (RevolutionContactsEditor *self,
            EContact                 *contact)
{
  g_autoptr (EContactName) name = e_contact_get (contact, E_CONTACT_NAME);
  g_autofree char *given = stripped (self->given_row);
  g_autofree char *family = stripped (self->family_row);
  g_autofree char *old_file_as = NULL;
  g_autofree char *full = NULL;
  const char *file_as = e_contact_get_const (contact, E_CONTACT_FILE_AS);
  gboolean changed;

  if (name == NULL)
    name = e_contact_name_new ();
  changed = g_strcmp0 (name->given, given) != 0 || g_strcmp0 (name->family, family) != 0;
  old_file_as = file_as_for (name->given, name->family);
  /* Bestehendes „Ablegen unter“ in eigener Form bleibt (Evolution behält den gewählten Stil). */
  file_as = (file_as == NULL || *file_as == '\0' || g_strcmp0 (file_as, old_file_as) == 0) ? NULL : file_as;

  g_free (name->given);
  name->given = g_strdup (given);
  g_free (name->family);
  name->family = g_strdup (family);
  e_contact_set (contact, E_CONTACT_NAME, name);

  if (!changed && !is_blank (e_contact_get_const (contact, E_CONTACT_FULL_NAME)))
    return;
  full = g_strstrip (g_strjoin (" ", given != NULL ? given : "", family != NULL ? family : "", NULL));
  if (*full == '\0')
    {
      /* Ohne Namen: Firma, sonst erste Adresse (Gültigkeit verlangt eins davon). */
      g_free (full);
      full = g_strdup (e_contact_get_const (contact, E_CONTACT_ORG));
      if (full == NULL)
        full = e_contact_get (contact, E_CONTACT_EMAIL_1);
    }
  e_contact_set (contact, E_CONTACT_FULL_NAME, full);
  if (file_as == NULL)
    {
      g_autofree char *new_file_as = file_as_for (given, family);

      e_contact_set (contact, E_CONTACT_FILE_AS, new_file_as != NULL ? new_file_as : full);
    }
}

static void
build_values (RevolutionContactsEditor *self,
              EContact                 *contact,
              gboolean                  email)
{
  GPtrArray *rows = email ? self->email_rows : self->phone_rows;
  GList *attrs = NULL;

  for (guint i = 0; i < rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (rows, i);
      ValueRow *data = g_object_get_data (G_OBJECT (row), "value");
      g_autofree char *value = stripped (row);
      guint type = gtk_drop_down_get_selected (data->type);
      EVCardAttribute *attr;

      if (value == NULL)
        continue;
      attr = data->attr != NULL ? e_vcard_attribute_copy (data->attr)
                                : e_vcard_attribute_new (NULL, email ? EVC_EMAIL : EVC_TEL);
      e_vcard_attribute_remove_values (attr);
      e_vcard_attribute_add_value (attr, value);
      if (data->attr == NULL || type != data->initial)
        {
          e_vcard_attribute_remove_param (attr, EVC_TYPE);
          e_vcard_attribute_add_param_with_value (attr, e_vcard_attribute_param_new (EVC_TYPE), data->types[type].type_1);
          if (data->types[type].type_2 != NULL)
            e_vcard_attribute_add_param_with_value (attr, e_vcard_attribute_param_new (EVC_TYPE),
                                                    data->types[type].type_2);
        }
      attrs = g_list_append (attrs, attr);
    }
  e_contact_set_attributes (contact, email ? E_CONTACT_EMAIL : E_CONTACT_TEL, attrs);
  g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);
}

/* Übernommen aus Evolution 3.56.2 src/addressbook/gui/contact-editor/e-contact-editor.c:2412-2469
 * (append_to_address_label, set_address_label ohne address-formatting). */
static char *
append_to_address_label (char       *address_label,
                         const char *part,
                         gboolean    newline)
{
  char *new_address_label;

  if (part == NULL || *part == '\0')
    return address_label;

  if (address_label)
    new_address_label = g_strjoin (newline ? "\n" : ", ", address_label, part, NULL);
  else
    new_address_label = g_strdup (part);

  g_free (address_label);
  return new_address_label;
}

static char *
address_label (const EContactAddress *address)
{
  char *label = NULL;

  label = append_to_address_label (label, address->street, TRUE);
  label = append_to_address_label (label, address->ext, TRUE);
  label = append_to_address_label (label, address->locality, TRUE);
  label = append_to_address_label (label, address->region, FALSE);
  label = append_to_address_label (label, address->code, TRUE);
  label = append_to_address_label (label, address->po, TRUE);
  label = append_to_address_label (label, address->country, TRUE);
  return label;
}
/* Ende der Übernahme. */

static void
replace (char      **field,
         GtkEditable *editable)
{
  g_free (*field);
  *field = stripped (editable);
}

static gboolean
same_address (const EContactAddress *a,
              const EContactAddress *b)
{
  return a != NULL && b != NULL && g_strcmp0 (a->street, b->street) == 0 && g_strcmp0 (a->code, b->code) == 0 &&
         g_strcmp0 (a->locality, b->locality) == 0 && g_strcmp0 (a->region, b->region) == 0 &&
         g_strcmp0 (a->country, b->country) == 0 && g_strcmp0 (a->po, b->po) == 0 && g_strcmp0 (a->ext, b->ext) == 0;
}

/* Je Typ: unveränderte Adresse und ihr Label bleiben, geänderte bekommen ein neues Label, entfernte fallen weg.
 * Typen ohne Adresse im Original und im Formular (z. B. nur ein Label) bleiben unberührt. */
static void
build_addresses (RevolutionContactsEditor *self,
                 EContact                 *contact)
{
  for (guint t = 0; t < G_N_ELEMENTS (address_types); t++)
    {
      g_autoptr (EContactAddress) original = e_contact_get (contact, address_types[t].address);
      g_autoptr (EContactAddress) address = NULL;
      g_autofree char *label = NULL;
      AddressRow *data = NULL;

      for (guint i = 0; data == NULL && i < self->address_rows->len; i++)
        {
          AddressRow *candidate = g_object_get_data (g_ptr_array_index (self->address_rows, i), "address");

          if (gtk_drop_down_get_selected (candidate->type) == t)
            data = candidate;
        }
      if (data != NULL)
        {
          address = data->original != NULL ? g_boxed_copy (E_TYPE_CONTACT_ADDRESS, data->original) : e_contact_address_new ();
          replace (&address->street, data->street);
          replace (&address->code, data->code);
          replace (&address->locality, data->locality);
          replace (&address->region, data->region);
          replace (&address->country, data->country);
          if (address->street == NULL && address->code == NULL && address->locality == NULL &&
              address->region == NULL && address->country == NULL && address->po == NULL && address->ext == NULL)
            g_clear_pointer (&address, e_contact_address_free);
        }
      if (address == NULL && original == NULL)
        continue;
      if (same_address (address, original))
        continue;
      label = address != NULL ? address_label (address) : NULL;
      e_contact_set (contact, address_types[t].address, address);
      e_contact_set (contact, address_types[t].label_field, label);
    }
}

static void
set_field (EContact      *contact,
           EContactField  field,
           gpointer       editable)
{
  g_autofree char *value = stripped (editable);

  if (g_strcmp0 (value, e_contact_get_const (contact, field)) != 0)
    e_contact_set (contact, field, value);
}

/* Neuer EContact: Kopie des Originals mit den Feldern des Formulars (UID und Unbekanntes bleiben). */
EContact *
revolution_contacts_editor_build (RevolutionContactsEditor *self)
{
  EContact *contact;
  GtkTextBuffer *buffer;
  GtkTextIter start, end;
  g_autofree char *note = NULL;

  g_return_val_if_fail (REVOLUTION_IS_CONTACTS_EDITOR (self), NULL);

  contact = e_contact_duplicate (self->original);
  build_values (self, contact, TRUE);
  build_values (self, contact, FALSE);
  build_addresses (self, contact);
  set_field (contact, E_CONTACT_ORG, self->org_row);
  set_field (contact, E_CONTACT_ORG_UNIT, self->unit_row);
  set_field (contact, E_CONTACT_TITLE, self->title_row);
  /* Name zuletzt: ohne Namen gilt Firma oder erste Adresse als voller Name. */
  build_name (self, contact);

  buffer = gtk_text_view_get_buffer (self->note_view);
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  note = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  if (is_blank (note))
    g_clear_pointer (&note, g_free);
  if (g_strcmp0 (note, e_contact_get_const (contact, E_CONTACT_NOTE)) != 0)
    e_contact_set (contact, E_CONTACT_NOTE, note);
  return contact;
}

guint
revolution_contacts_editor_get_book (RevolutionContactsEditor *self)
{
  g_return_val_if_fail (REVOLUTION_IS_CONTACTS_EDITOR (self), 0);

  return adw_combo_row_get_selected (self->book_row);
}

gboolean
revolution_contacts_editor_is_valid (RevolutionContactsEditor *self)
{
  g_return_val_if_fail (REVOLUTION_IS_CONTACTS_EDITOR (self), FALSE);

  return self->valid;
}

static void
revolution_contacts_editor_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  RevolutionContactsEditor *self = REVOLUTION_CONTACTS_EDITOR (object);

  if (prop_id == PROP_VALID)
    g_value_set_boolean (value, self->valid);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_contacts_editor_dispose (GObject *object)
{
  RevolutionContactsEditor *self = REVOLUTION_CONTACTS_EDITOR (object);

  g_clear_object (&self->original);
  g_clear_pointer (&self->email_rows, g_ptr_array_unref);
  g_clear_pointer (&self->phone_rows, g_ptr_array_unref);
  g_clear_pointer (&self->address_rows, g_ptr_array_unref);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_CONTACTS_EDITOR);

  G_OBJECT_CLASS (revolution_contacts_editor_parent_class)->dispose (object);
}

static void
revolution_contacts_editor_class_init (RevolutionContactsEditorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = revolution_contacts_editor_get_property;
  object_class->dispose = revolution_contacts_editor_dispose;

  props[PROP_VALID] = g_param_spec_boolean ("valid", NULL, NULL, FALSE,
                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, N_PROPS, props);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/contacts-editor.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, scroller);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, book_group);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, book_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, given_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, family_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, email_group);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, phone_group);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, address_group);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, add_address_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, org_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, unit_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, title_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsEditor, note_view);
  gtk_widget_class_bind_template_callback (widget_class, on_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_add_email);
  gtk_widget_class_bind_template_callback (widget_class, on_add_phone);
  gtk_widget_class_bind_template_callback (widget_class, on_add_address);
}

static void
revolution_contacts_editor_init (RevolutionContactsEditor *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
  self->email_rows = g_ptr_array_new ();
  self->phone_rows = g_ptr_array_new ();
  self->address_rows = g_ptr_array_new ();
}
