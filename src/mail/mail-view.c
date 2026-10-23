/* mail-view.c – Mail-Ansicht (Prototyp #336)
 *
 * Vorlage: quelltext/stamp/src/views/mail/stamp-mail-view.c (Template-Klasse
 * mit Eigenschaft „stack“ für den Ansichtsumschalter). Oberfläche in
 * mail-view.blp; hier Template-Anbindung und seit #345 die Konten der Seitenleiste.
 * Ordner und Nachrichten füllen #348 und #352. Aktionen ohne Funktion sind nicht
 * registriert, ihre Knöpfe und Menüeinträge sind daher ausgegraut (#342).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include "composer/composer.h"
#include "mail-account.h"
#include "mail-sample.h"
#include "mail-view.h"

struct _RevolutionMailView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  ESourceRegistry     *registry;
  GListStore          *accounts;
  GtkStack            *sidebar_stack;
  AdwBanner           *banner;
  GtkSingleSelection  *folder_selection;
  GtkSingleSelection  *message_selection;
  GtkStack            *header_stack;
  GtkSearchEntry      *search_entry;
  GtkToggleButton     *search_button;
  AdwOverlaySplitView *split;
};

G_DEFINE_FINAL_TYPE (RevolutionMailView, revolution_mail_view, ADW_TYPE_BREAKPOINT_BIN)

/* list.compose öffnet den Composer (#337) als eigenes Fenster wie Stamp. */
static void
on_compose (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (user_data));

  gtk_window_present (revolution_composer_new (gtk_window_get_application (GTK_WINDOW (root))));
}

static const GActionEntry list_actions[] = {
  { "compose", on_compose, NULL, NULL, NULL, { 0 } },
};

/* Fokus ins Suchfeld ohne alles zu markieren: sonst ersetzt beim Tippen in der
 * Liste (key-capture-widget) der zweite Buchstabe den ersten. */
static void
focus_search_entry (RevolutionMailView *self)
{
  GtkEditable *text = gtk_editable_get_delegate (GTK_EDITABLE (self->search_entry));

  gtk_text_grab_focus_without_selecting (GTK_TEXT (text));
}

/* #399 Punkt 5: Suchknopf schaltet die Kopfleiste der Nachricht zwischen den
 * Aktionen und dem Suchfeld um („Lights off“), Fokus wandert mit. */
static void
on_search_toggled (RevolutionMailView *self)
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

/* Tippen in der Listenspalte (GtkSearchEntry::search-started) öffnet die Suche. */
static void
on_search_started (RevolutionMailView *self)
{
  gtk_toggle_button_set_active (self->search_button, TRUE);
}

/* Escape im Suchfeld (GtkSearchEntry::stop-search). */
static void
on_search_stop (RevolutionMailView *self)
{
  gtk_toggle_button_set_active (self->search_button, FALSE);
}

/* mail.search für Strg+F: öffnet nur, schließt nicht (Schließen: Escape, Suchknopf).
 * Ist die Suche schon offen, holt es den Fokus zurück ins Suchfeld. */
static void
on_search (GSimpleAction *action G_GNUC_UNUSED,
           GVariant      *parameter G_GNUC_UNUSED,
           gpointer       user_data)
{
  RevolutionMailView *self = user_data;

  if (gtk_toggle_button_get_active (self->search_button))
    focus_search_entry (self);
  else
    gtk_toggle_button_set_active (self->search_button, TRUE);
}

/* mail.toggle-sidebar fürs Hauptmenü (#401 Punkt 2): wie mail.sidebar (F9), aber
 * zustandslos, damit GtkPopoverMenu keine Haken-Spalte einrückt. */
static void
on_toggle_sidebar (GSimpleAction *action G_GNUC_UNUSED,
                   GVariant      *parameter G_GNUC_UNUSED,
                   gpointer       user_data)
{
  RevolutionMailView *self = user_data;

  adw_overlay_split_view_set_show_sidebar (self->split, !adw_overlay_split_view_get_show_sidebar (self->split));
}

static const GActionEntry mail_actions[] = {
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  { "toggle-sidebar", on_toggle_sidebar, NULL, NULL, NULL, { 0 } },
};

/* #345: Eingebaute Speicher, die Evolution neben den Konten im Ordnerbaum zeigt:
 * „Auf diesem Rechner“ (local) und „Suchordner“ (vfolder), Evolution e-mail-session.h:58-59
 * (E_MAIL_SESSION_LOCAL_UID, E_MAIL_SESSION_VFOLDER_UID), sowie „Nachrichten und Blogs“
 * (rss, Evolution-Modul rss, e-rss-folder-tree-model-extension.c:66). Revolution hat für
 * keinen davon Funktionen; sie erscheinen erst mit einem eigenen Task. */
static const char * const builtin_uids[] = { "local", "vfolder", "rss", NULL };

/* #345: Welche Mail-Konten Evolution 3.56.2 im Ordnerbaum zeigt. NULL = angezeigt,
 * sonst der Grund fürs Log.
 * 1. Konto unter GNOME/Ubuntu Online Accounts, dessen Mail-Teil aus ist (die Quelle selbst
 *    „disabled“): erscheint nirgends (e-mail-session.c:515-555, 595-600).
 * 2. Sonst entscheidet „enabled“ der umgebenden Collection, ohne Collection die Quelle
 *    selbst (e-mail-account-store.c:1220-1255); der Ordnerbaum zeigt nur aktivierte
 *    (em-folder-tree-model.c:559-574).
 * Die Registry-Aufrufe lesen nur die Quellen im Speicher (EDS e-source-registry.c:459-472,
 * 2576-2604), kein D-Bus; wie on_registry_loaded in main.c im UI-Thread. */
static const char *
account_hidden_reason (ESourceRegistry *registry,
                       ESource         *source)
{
  g_autoptr (ESource) goa = NULL;
  g_autoptr (ESource) uoa = NULL;
  g_autoptr (ESource) collection = NULL;

  if (g_strv_contains (builtin_uids, e_source_get_uid (source)))
    return "eingebauter Speicher";

  goa = e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_GOA);
  uoa = e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_UOA);
  if ((goa != NULL || uoa != NULL) && !e_source_get_enabled (source))
    return "Online-Konto, Mail aus";

  collection = e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_COLLECTION);
  if (!e_source_get_enabled (collection != NULL ? collection : source))
    return collection != NULL ? "Collection deaktiviert" : "deaktiviert";

  return NULL;
}

/* #345: Konten aus der Registry in den Store, nur im UI-Thread (Registry-Signale kommen
 * dort an, README „Worker“). Reihenfolge alphabetisch nach Anzeigename wie Evolutions
 * Standard (e-mail-account-store.c:119-153, g_utf8_collate); e_source_registry_list_sources
 * sortiert schon so (EDS e-source-registry.c:2474-2476, e-source.c:3368-3375). Evolutions
 * eigene Reihenfolge (sortorder.ini, e-mail-account-store.c:565) liest Revolution nicht
 * (Architektur-Festlegung 4). Bleiben die Konten gleich, ändert sich nur der Name: die
 * Einträge und damit Auswahl und Zeilen bleiben erhalten. */
static void
sync_accounts (RevolutionMailView *self)
{
  g_autolist (ESource) sources = e_source_registry_list_sources (self->registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
  g_autoptr (GPtrArray) shown = g_ptr_array_new ();
  GListModel *model = G_LIST_MODEL (self->accounts);
  guint n_items = g_list_model_get_n_items (model);
  gboolean same;

  for (GList *l = sources; l != NULL; l = l->next)
    {
      const char *reason = account_hidden_reason (self->registry, l->data);
      g_autofree char *name = e_source_dup_display_name (l->data);

      g_debug ("Mail-Konto %s „%s“: %s", e_source_get_uid (l->data), name, reason ? reason : "angezeigt");
      if (reason == NULL)
        g_ptr_array_add (shown, l->data);
    }

  same = shown->len == n_items;
  for (guint i = 0; same && i < n_items; i++)
    {
      g_autoptr (RevolutionMailAccount) account = g_list_model_get_item (model, i);

      same = g_strcmp0 (revolution_mail_account_get_uid (account), e_source_get_uid (shown->pdata[i])) == 0;
    }

  if (same)
    {
      for (guint i = 0; i < n_items; i++)
        {
          g_autoptr (RevolutionMailAccount) account = g_list_model_get_item (model, i);
          g_autofree char *name = e_source_dup_display_name (shown->pdata[i]);

          revolution_mail_account_set_display_name (account, name);
        }
    }
  else
    {
      g_autoptr (GPtrArray) accounts = g_ptr_array_new_with_free_func (g_object_unref);

      for (guint i = 0; i < shown->len; i++)
        {
          g_autofree char *name = e_source_dup_display_name (shown->pdata[i]);

          g_ptr_array_add (accounts, revolution_mail_account_new (e_source_get_uid (shown->pdata[i]), name));
        }
      g_list_store_splice (self->accounts, 0, n_items, accounts->pdata, accounts->len);
    }

  g_debug ("Seitenleiste: %u von %u Mail-Konten angezeigt", shown->len, g_list_length (sources));
  gtk_stack_set_visible_child_name (self->sidebar_stack, shown->len > 0 ? "list" : "empty");
}

/* create_func des GtkTreeListModel (Vorlage Tag m2-prototyp mail-sample.c:68-76):
 * Konten haben bis #348 keine Ordner; NULL heißt „keine Kinder, kein Ausklapp-Pfeil“. */
static GListModel *
account_children (gpointer item G_GNUC_UNUSED,
                  gpointer user_data G_GNUC_UNUSED)
{
  return NULL;
}

/* #401 Punkt 1: Klasse .dark für Regeln nur im dunklen Design (style.css). */
static void
on_dark_changed (AdwStyleManager    *manager,
                 GParamSpec         *pspec G_GNUC_UNUSED,
                 RevolutionMailView *self)
{
  if (adw_style_manager_get_dark (manager))
    gtk_widget_add_css_class (GTK_WIDGET (self), "dark");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "dark");
}

enum { PROP_0, PROP_STACK, N_PROPS };

static void
revolution_mail_view_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  if (prop_id == PROP_STACK)
    g_value_set_object (value, self->stack);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_mail_view_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  if (prop_id == PROP_STACK)
    g_set_object (&self->stack, g_value_get_object (value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_mail_view_dispose (GObject *object)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
  g_clear_object (&self->stack);
  g_clear_object (&self->registry);
  g_clear_object (&self->accounts);

  G_OBJECT_CLASS (revolution_mail_view_parent_class)->dispose (object);
}

static void
revolution_mail_view_class_init (RevolutionMailViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = revolution_mail_view_get_property;
  object_class->set_property = revolution_mail_view_set_property;
  object_class->dispose = revolution_mail_view_dispose;

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_MAIL_ACCOUNT);
  g_type_ensure (REVOLUTION_TYPE_SAMPLE_MESSAGE);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, sidebar_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, banner);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, split);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
}

static void
revolution_mail_view_init (RevolutionMailView *self)
{
  g_autoptr (GtkTreeListModel) tree = NULL;
  g_autoptr (GListStore) messages = g_list_store_new (REVOLUTION_TYPE_SAMPLE_MESSAGE);
  g_autoptr (GSimpleActionGroup) list_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) mail_group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  /* CR #397: Ordnerbaum als GtkTreeListModel, Konten als oberste Ebene (#345). */
  self->accounts = g_list_store_new (REVOLUTION_TYPE_MAIL_ACCOUNT);
  tree = gtk_tree_list_model_new (g_object_ref (G_LIST_MODEL (self->accounts)), FALSE, TRUE,
                                  account_children, NULL, NULL);
  gtk_single_selection_set_model (self->folder_selection, G_LIST_MODEL (tree));
  gtk_single_selection_set_model (self->message_selection, G_LIST_MODEL (messages));

  g_action_map_add_action_entries (G_ACTION_MAP (list_group), list_actions, G_N_ELEMENTS (list_actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "list", G_ACTION_GROUP (list_group));
  g_action_map_add_action_entries (G_ACTION_MAP (mail_group), mail_actions, G_N_ELEMENTS (mail_actions), self);
  /* mail.sidebar für F9 wie calendar.sidebar (calendar-view.c:426-431). */
  sidebar_action = g_property_action_new ("sidebar", self->split, "show-sidebar");
  g_action_map_add_action (G_ACTION_MAP (mail_group), G_ACTION (sidebar_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "mail", G_ACTION_GROUP (mail_group));

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}

/* #345: Aufruf aus main.c, sobald die Registry im Worker geladen ist (UI-Thread).
 * Live wie Evolution: neue, entfernte, (de)aktivierte Quellen (e-mail-session.c:1268-1288)
 * und Umbenennen (Bindung an display-name, e-mail-session.c:1410-1413) aktualisieren die
 * Liste. Den Namen meldet hier „source-changed“ im UI-Thread (EDS e-source-registry.c:
 * 580-598); ESource:display-name selbst ändert sich im D-Bus-Thread der Registry
 * (e-source-registry.c:1051, e-source.c:792-803) und wird daher nicht gebunden. */
void
revolution_mail_view_set_registry (RevolutionMailView *self,
                                   ESourceRegistry    *registry)
{
  const char *signals[] = { "source-added", "source-removed", "source-enabled", "source-disabled", "source-changed" };

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));
  g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
  g_return_if_fail (self->registry == NULL);

  self->registry = g_object_ref (registry);
  for (gsize i = 0; i < G_N_ELEMENTS (signals); i++)
    g_signal_connect_object (registry, signals[i], G_CALLBACK (sync_accounts), self, G_CONNECT_SWAPPED);
  sync_accounts (self);
}

/* #343/#345: Fehler im AdwBanner der Mail-Ansicht (mail-view.blp), kurzer Titel,
 * Einzelheiten im Tooltip. */
void
revolution_mail_view_show_error (RevolutionMailView *self,
                                 const char         *title,
                                 const char         *detail)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  adw_banner_set_title (self->banner, title);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->banner), detail);
  adw_banner_set_revealed (self->banner, TRUE);
}
