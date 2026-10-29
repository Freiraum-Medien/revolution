/* mail-folder-actions.c – Ordner-Menü und Ordnerverwaltung der Mail-Ansicht (#364).
 * Aus mail-view.c (#444); gemeinsamer Zustand in mail-view-private.h.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>

#include "mail-favorites.h"
#include "mail-view-private.h"

/* #364: Ordner-Menü wie Evolution (e-mail-shell-view-actions.c:1664-1733), nur Ordner der Konten. Unterordner
 * nicht in Papierkorb/Spam und NOINFERIORS (e-mail-sidebar.c:475, e-mail-folder-create-dialog.c:332); Umbenennen
 * und Löschen nicht für Spezialordner (Task; Evolution nur SYSTEM, e-mail-sidebar.c:477-488) und NOSELECT. */
enum { EDIT_NEW, EDIT_RENAME, EDIT_DELETE };

static const struct { const char *action, *done, *title; } folder_ops[] = {
  { "new", "angelegt", N_("Folder Not Created") },
  { "rename", "umbenannt", N_("Folder Not Renamed") },
  { "delete", "gelöscht", N_("Folder Not Deleted") },
};

typedef struct
{
  RevolutionMailView *self;
  CamelStore         *store;
  ESourceRegistry    *registry;
  guint               op;
  char               *uid, *full_name, *name, *result;   /* Neu: full_name = übergeordneter Ordner oder NULL */
  CamelFolderInfo    *info;
} FolderEdit;

static void
folder_edit_free (FolderEdit *edit)
{
  g_object_unref (edit->self);
  g_object_unref (edit->store);
  g_object_unref (edit->registry);
  g_free (edit->uid);
  g_free (edit->full_name);
  g_free (edit->name);
  g_free (edit->result);
  g_clear_pointer (&edit->info, camel_folder_info_free);
  g_free (edit);
}

static RevolutionMailAccount *
folder_store_account (RevolutionMailView *self)
{
  g_autoptr (RevolutionMailAccount) account = NULL;

  if (self->folder != NULL && self->session != NULL)
    g_object_get (self->folder, "account", &account, NULL);

  return account != self->favorites && account != self->local ? g_steal_pointer (&account) : NULL;
}

void
update_folder_actions (RevolutionMailView *self)
{
  g_autoptr (RevolutionMailAccount) account = folder_store_account (self);
  guint32 flags = self->folder != NULL ? revolution_mail_folder_get_flags (self->folder) : 0;
  gboolean fixed = (flags & (CAMEL_FOLDER_TYPE_MASK | CAMEL_FOLDER_SYSTEM | CAMEL_FOLDER_NOSELECT)) != 0;

  for (guint i = 0; i < G_N_ELEMENTS (folder_ops); i++)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (self->folder_actions, folder_ops[i].action)),
                                 account != NULL && (i == EDIT_NEW || !fixed));
}

static void
folder_edit_thread (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
  FolderEdit *edit = task_data;
  GError *error = NULL;
  gboolean done;

  if (edit->op == EDIT_NEW)
    done = revolution_mail_folder_create_sync (source_object, edit->full_name, edit->name, &edit->result, cancellable, &error);
  else if (edit->op == EDIT_RENAME)
    done = revolution_mail_folder_rename_sync (source_object, edit->full_name, edit->name, &edit->result, cancellable, &error);
  else
    done = revolution_mail_folder_delete_sync (source_object, edit->full_name, cancellable, &error);

  /* Danach (auch nach Fehler) Baum = Server: kein verwaister Eintrag; Zähler bleiben. Spezialordner wie beim
   * Start (main.c), sonst erscheinen die virtuellen Papierkorb/Spam wieder (#435). */
  edit->info = camel_store_get_folder_info_sync (source_object, NULL, CAMEL_STORE_FOLDER_INFO_RECURSIVE |
                                                 CAMEL_STORE_FOLDER_INFO_SUBSCRIBED, cancellable, NULL);
  if (edit->info != NULL)
    revolution_mail_folder_mark_special (source_object, edit->registry, edit->info, cancellable);
  revolution_mail_folder_forget_unread (edit->info);
  if (done)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
on_folder_edited (GObject      *source_object G_GNUC_UNUSED,
                  GAsyncResult *result,
                  gpointer      user_data G_GNUC_UNUSED)
{
  FolderEdit *edit = g_task_get_task_data (G_TASK (result));
  RevolutionMailView *self = edit->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autoptr (RevolutionMailFolder) folder = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    g_debug ("Ordner: %s %s nicht %s: %s", edit->uid, edit->full_name, folder_ops[edit->op].done, error->message);
  else
    g_debug ("Ordner: %s %s %s", edit->uid, folder_ops[edit->op].done, edit->result ? edit->result : edit->full_name);
  if (self->accounts == NULL)   /* nach dispose */
    return;
  if (edit->info != NULL)
    revolution_mail_view_update_folders (self, edit->uid, edit->info);
  if (error != NULL)
    revolution_mail_view_show_error (self, _(folder_ops[edit->op].title), error->message, FALSE);

  /* Neuen bzw. umbenannten Ordner wählen wie Evolution (mail_shell_view_folder_created_cb, em-folder-tree.c:715-717). */
  account = edit->result != NULL && error == NULL ? find_account (self, edit->uid) : NULL;
  folder = account != NULL ? revolution_mail_folder_find (account_folder_store (account), edit->result) : NULL;
  if (folder != NULL)
    select_folder (self, folder);
}

static void
on_folder_dialog_chosen (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  FolderEdit *edit = user_data;
  GtkEditable *entry = g_object_get_data (source_object, "entry");
  GtkCheckButton *check = g_object_get_data (source_object, "check");
  g_autoptr (GTask) task = NULL;

  if (g_strcmp0 (adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source_object), result), "ok") != 0)
    {
      folder_edit_free (edit);
      return;
    }

  if (entry != NULL)
    edit->name = g_strstrip (g_strdup (gtk_editable_get_text (entry)));
  if (check != NULL && !gtk_check_button_get_active (check))
    g_clear_pointer (&edit->full_name, g_free);
  task = g_task_new (edit->store, edit->self->cancellable, on_folder_edited, NULL);
  g_task_set_source_tag (task, on_folder_dialog_chosen);
  g_task_set_task_data (task, edit, (GDestroyNotify) folder_edit_free);
  g_task_run_in_thread (task, folder_edit_thread);
}

/* Name gültig wie Evolution: nicht leer, ohne „/“ (em-folder-tree.c:675-681). */
static void
on_folder_name_changed (GtkEditable    *entry,
                        AdwAlertDialog *dialog)
{
  g_autofree char *name = g_strstrip (g_strdup (gtk_editable_get_text (entry)));

  adw_alert_dialog_set_response_enabled (dialog, "ok", *name != '\0' && strchr (name, '/') == NULL);
}

/* folder.new/rename/delete: ein AdwAlertDialog, Auftrag nach „ok“. Löschen immer mit Bestätigung wie Evolution
 * (mail.error.xml:210-235, Vorgabe „Nicht löschen“, e-mail-reader-utils.c:283-301); neu im gewählten Ordner
 * (e-mail-shell-view-actions.c:1009-1014). */
void
on_folder_edit (GSimpleAction *action,
                GVariant      *parameter G_GNUC_UNUSED,
                gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  g_autoptr (RevolutionMailAccount) account = folder_store_account (self);
  g_autoptr (CamelService) store = NULL;
  const char *full_name = revolution_mail_folder_get_full_name (self->folder);
  const char *slash = strrchr (full_name, '/');
  guint32 flags = revolution_mail_folder_get_flags (self->folder);
  GListModel *children = revolution_mail_folder_get_children (self->folder);
  gboolean with_children = children != NULL && g_list_model_get_n_items (children) > 0;
  g_autofree char *display_name = NULL;
  g_autofree char *heading = NULL;
  AdwDialog *dialog;
  FolderEdit *edit;
  guint op = 0;

  while (g_strcmp0 (folder_ops[op].action, g_action_get_name (G_ACTION (action))) != 0)
    op++;
  store = account != NULL ? camel_session_ref_service (self->session, revolution_mail_account_get_uid (account)) : NULL;
  if (!CAMEL_IS_STORE (store))
    return;

  edit = g_new0 (FolderEdit, 1);
  edit->self = g_object_ref (self);
  edit->store = CAMEL_STORE (g_steal_pointer (&store));
  edit->registry = g_object_ref (self->registry);
  edit->op = op;
  edit->uid = g_strdup (revolution_mail_account_get_uid (account));
  edit->full_name = g_strdup (full_name);
  g_object_get (self->folder, "display-name", &display_name, NULL);

  if (op == EDIT_DELETE)
    heading = g_strdup_printf (with_children ? _("Delete Folder “%s” and All Subfolders?") : _("Delete Folder “%s”?"),
                               display_name);
  dialog = adw_alert_dialog_new (op == EDIT_NEW ? _("New Folder") : op == EDIT_RENAME ? _("Rename Folder") : heading,
                                 op != EDIT_DELETE ? NULL : with_children
                                 ? _("The contents of the folder and its subfolders will be permanently deleted.")
                                 : _("The contents of the folder will be permanently deleted."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", op == EDIT_DELETE ? _("Do _Not Delete") : _("_Cancel"),
                                  "ok", op == EDIT_NEW ? _("C_reate") : op == EDIT_RENAME ? _("_Rename") : _("_Delete"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "ok",
                                            op == EDIT_DELETE ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), op == EDIT_DELETE ? "cancel" : "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  if (op != EDIT_DELETE)
    {
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
      GtkWidget *entry = gtk_entry_new ();

      gtk_editable_set_text (GTK_EDITABLE (entry), op == EDIT_RENAME ? (slash != NULL ? slash + 1 : full_name) : "");
      gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
      gtk_accessible_update_property (GTK_ACCESSIBLE (entry), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Folder Name"), -1);
      gtk_box_append (GTK_BOX (box), entry);
      if (op == EDIT_NEW)
        {
          g_autofree char *label = g_strdup_printf (_("_Subfolder of “%s”"), display_name);
          GtkWidget *check = gtk_check_button_new_with_mnemonic (label);
          gboolean inside = (flags & CAMEL_FOLDER_NOINFERIORS) == 0 &&
                            (flags & CAMEL_FOLDER_TYPE_MASK) != CAMEL_FOLDER_TYPE_TRASH &&
                            (flags & CAMEL_FOLDER_TYPE_MASK) != CAMEL_FOLDER_TYPE_JUNK;

          gtk_check_button_set_active (GTK_CHECK_BUTTON (check), inside);
          gtk_widget_set_sensitive (check, inside);
          gtk_box_append (GTK_BOX (box), check);
          g_object_set_data (G_OBJECT (dialog), "check", check);
        }
      adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
      g_object_set_data (G_OBJECT (dialog), "entry", entry);
      g_signal_connect_object (entry, "changed", G_CALLBACK (on_folder_name_changed), dialog, G_CONNECT_DEFAULT);
      on_folder_name_changed (GTK_EDITABLE (entry), ADW_ALERT_DIALOG (dialog));
      adw_dialog_set_focus (dialog, entry);
    }

  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dialog), GTK_WIDGET (self), NULL, on_folder_dialog_chosen, edit);
}

/* Entf/⌫ im Ordnerbaum immer verbrauchen, sonst ginge sie bei gesperrtem folder.delete an message.delete. */
gboolean
on_folder_delete_key (GtkWidget *widget G_GNUC_UNUSED,
                      GVariant  *args G_GNUC_UNUSED,
                      gpointer   user_data)
{
  RevolutionMailView *self = user_data;

  if (g_action_group_get_action_enabled (G_ACTION_GROUP (self->folder_actions), "delete"))
    g_action_group_activate_action (G_ACTION_GROUP (self->folder_actions), "delete", NULL);

  return TRUE;
}

void
on_folder_secondary_click (GtkListItem     *item,
                           int              n_press G_GNUC_UNUSED,
                           double           x,
                           double           y,
                           GtkGestureClick *gesture)
{
  RevolutionMailView *self = view_for (GTK_EVENT_CONTROLLER (gesture));

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_single_selection_set_selected (self->folder_selection, gtk_list_item_get_position (item));
  show_row_menu (self, gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture)), x, y);
}
