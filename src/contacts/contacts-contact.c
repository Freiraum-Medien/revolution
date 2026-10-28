/* contacts-contact.c – Eintrag der Kontaktliste (#368)
 *
 * Hält den EContact aus der EBookClientView (contacts-view.c) und liefert die Eigenschaften, an die die
 * Zeilenvorlage in contacts-view.blp bindet: Name, Anfangsbuchstabe (Abschnitt) und Foto.
 * Vorlage: quelltext/stamp/src/views/contact/contact-list/stamp-contact-item.c (EContact im Eintrag) und
 * stamp-contact-row.c:200-230 (Avatar mit Initialen, Foto als custom-image). Name: voller Name, sonst
 * „Ablegen unter“, sonst Firma, sonst erste Adresse (Rückfall auf die Adresse wie stamp-contact-row.c:156-176).
 *
 * Foto (E_CONTACT_PHOTO): eingebettet (Bytes) oder als URI (EBookMetaBackend legt Fotos der Server-Bücher als
 * Datei im Cache ab). Geladen wird erst, wenn die Zeile das Foto liest (gebundene, sichtbare Zeile), im
 * Worker-GTask: gdk_texture_new_from_bytes/_from_file sind threadsicher („This function is threadsafe, so that
 * you can e.g. use GTask“, /usr/share/gir-1.0/Gdk-4.0.gir:33602, 33638); Datei-Zugriff nie im UI-Thread.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "contacts-contact.h"

struct _RevolutionContactsContact
{
  GObject       parent_instance;
  EContact     *contact;
  char         *name;
  char         *initial;
  GdkPaintable *photo;
  gboolean      photo_requested;
  guint         generation;  /* verwirft ein Foto, das zu einem älteren EContact gehört */
};

G_DEFINE_FINAL_TYPE (RevolutionContactsContact, revolution_contacts_contact, G_TYPE_OBJECT)

enum { PROP_0, PROP_NAME, PROP_INITIAL, PROP_PHOTO, N_PROPS };

static GParamSpec *props[N_PROPS];

static void
photo_thread (GTask        *task,
              gpointer      source_object G_GNUC_UNUSED,
              gpointer      task_data,
              GCancellable *cancellable G_GNUC_UNUSED)
{
  EContactPhoto *photo = task_data;
  GError *error = NULL;
  GdkTexture *texture = NULL;

  if (photo->type == E_CONTACT_PHOTO_TYPE_INLINED)
    {
      g_autoptr (GBytes) bytes = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);

      texture = gdk_texture_new_from_bytes (bytes, &error);
    }
  else if (photo->data.uri != NULL)
    {
      g_autoptr (GFile) file = g_file_new_for_uri (photo->data.uri);

      texture = gdk_texture_new_from_file (file, &error);
    }
  if (texture == NULL && error == NULL)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no photo data");
  if (texture == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, texture, g_object_unref);
}

static void
on_photo_loaded (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  RevolutionContactsContact *self = REVOLUTION_CONTACTS_CONTACT (source_object);
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) texture = g_task_propagate_pointer (G_TASK (result), &error);

  if (GPOINTER_TO_UINT (user_data) != self->generation)
    return;
  if (texture == NULL)
    {
      g_debug ("Kontakte: Foto %s nicht geladen: %s", revolution_contacts_contact_get_uid (self), error->message);
      return;
    }
  g_set_object (&self->photo, GDK_PAINTABLE (texture));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PHOTO]);
}

/* Erst beim ersten Lesen (Bindung der Zeile) im Worker laden. */
static GdkPaintable *
get_photo (RevolutionContactsContact *self)
{
  EContactPhoto *photo;
  g_autoptr (GTask) task = NULL;

  if (self->photo_requested)
    return self->photo;
  self->photo_requested = TRUE;
  photo = e_contact_get (self->contact, E_CONTACT_PHOTO);
  if (photo == NULL)
    return NULL;
  task = g_task_new (self, NULL, on_photo_loaded, GUINT_TO_POINTER (self->generation));
  g_task_set_source_tag (task, get_photo);
  g_task_set_task_data (task, photo, (GDestroyNotify) e_contact_photo_free);
  g_task_run_in_thread (task, photo_thread);
  return NULL;
}

static void
revolution_contacts_contact_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  RevolutionContactsContact *self = REVOLUTION_CONTACTS_CONTACT (object);

  switch (prop_id)
    {
    case PROP_NAME: g_value_set_string (value, self->name); break;
    case PROP_INITIAL: g_value_set_string (value, self->initial); break;
    case PROP_PHOTO: g_value_set_object (value, get_photo (self)); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_contacts_contact_finalize (GObject *object)
{
  RevolutionContactsContact *self = REVOLUTION_CONTACTS_CONTACT (object);

  g_clear_object (&self->contact);
  g_clear_object (&self->photo);
  g_free (self->name);
  g_free (self->initial);

  G_OBJECT_CLASS (revolution_contacts_contact_parent_class)->finalize (object);
}

static void
revolution_contacts_contact_class_init (RevolutionContactsContactClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY;

  object_class->get_property = revolution_contacts_contact_get_property;
  object_class->finalize = revolution_contacts_contact_finalize;

  props[PROP_NAME] = g_param_spec_string ("name", NULL, NULL, NULL, flags);
  props[PROP_INITIAL] = g_param_spec_string ("initial", NULL, NULL, NULL, flags);
  props[PROP_PHOTO] = g_param_spec_object ("photo", NULL, NULL, GDK_TYPE_PAINTABLE, flags);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
revolution_contacts_contact_init (RevolutionContactsContact *self G_GNUC_UNUSED)
{
}

/* Abschnitt: erster Buchstabe ohne diakritische Zeichen (Ä unter A), Großbuchstabe; sonst „#“. */
static char *
initial_of (const char *name)
{
  g_autofree char *base = g_utf8_normalize (name, -1, G_NORMALIZE_NFD);
  gunichar c = base != NULL && *base != '\0' ? g_utf8_get_char (base) : 0;
  char out[7] = { 0 };

  if (!g_unichar_isalpha (c))
    return g_strdup ("#");
  g_unichar_to_utf8 (g_unichar_toupper (c), out);
  return g_strdup (out);
}

void
revolution_contacts_contact_set_contact (RevolutionContactsContact *self,
                                         EContact                  *contact)
{
  const EContactField fields[] = { E_CONTACT_FULL_NAME, E_CONTACT_FILE_AS, E_CONTACT_ORG, E_CONTACT_EMAIL_1 };
  const char *name = NULL;

  g_return_if_fail (REVOLUTION_IS_CONTACTS_CONTACT (self));

  g_set_object (&self->contact, contact);
  for (gsize i = 0; i < G_N_ELEMENTS (fields) && (name == NULL || *name == '\0'); i++)
    name = e_contact_get_const (contact, fields[i]);
  g_free (self->name);
  self->name = g_strstrip (g_strdup (name != NULL && *name != '\0' ? name : _("Unnamed Contact")));
  g_free (self->initial);
  self->initial = initial_of (self->name);
  self->generation++;
  self->photo_requested = FALSE;
  g_clear_object (&self->photo);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NAME]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_INITIAL]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PHOTO]);
}

RevolutionContactsContact *
revolution_contacts_contact_new (EContact *contact)
{
  RevolutionContactsContact *self = g_object_new (REVOLUTION_TYPE_CONTACTS_CONTACT, NULL);

  revolution_contacts_contact_set_contact (self, contact);
  return self;
}

const char *
revolution_contacts_contact_get_uid (RevolutionContactsContact *self)
{
  return e_contact_get_const (self->contact, E_CONTACT_UID);
}

const char *
revolution_contacts_contact_get_name (RevolutionContactsContact *self)
{
  return self->name;
}

const char *
revolution_contacts_contact_get_initial (RevolutionContactsContact *self)
{
  return self->initial;
}
