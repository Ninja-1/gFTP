#include <geanyplugin.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "ftpparse.c"

GeanyPlugin *geany_plugin;
GeanyData *geany_data;
GeanyFunctions *geany_functions;

PLUGIN_VERSION_CHECK(211)

PLUGIN_SET_INFO("gFTP", "FTP", "1.0", "Cai Guanhao <caiguanhao@gmail.com>");

enum
{
	FILEVIEW_COLUMN_ICON = 0,
	FILEVIEW_COLUMN_NAME,
	FILEVIEW_COLUMN_SIZE,
	FILEVIEW_COLUMN_FILENAME,
	FILEVIEW_N_COLUMNS
};

static GtkWidget *box;
static GtkWidget *file_view;
static GtkWidget *url_entry;
static GtkWidget *btn_connect;
static GtkTreeStore *file_store;
static GtkTreeIter parent;
static CURL *curl;
static gchar *current_url = NULL;
static gchar **all_profiles;
static gsize all_profiles_length;
static gchar *profiles_file;

GList *filelist;
GList *dirlist;

struct string
{
	char *ptr;
	int len;
};

static struct
{
	GtkListStore *store;
	GtkTreeIter iter_store_new;
	GtkWidget *combo;
	GtkWidget *delete;
	GtkWidget *host;
	GtkWidget *port;
	GtkWidget *login;
	GtkWidget *passwd;
	GtkWidget *anon;
	GtkWidget *showpass;
} pref;

size_t write_function(void *ptr, size_t size, size_t nmemb, struct string *str)
{
	int new_len = str->len + size * nmemb;
	str->ptr = realloc(str->ptr, new_len + 1);
	memcpy(str->ptr + str->len, ptr, size * nmemb);
	str->ptr[new_len] = '\0';
	str->len = new_len;
	return size*nmemb;
}

size_t write_data (void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	return fwrite(ptr, size, nmemb, stream);
}

static int download_progress (void *p, double dltotal, double dlnow, double ultotal, double ulnow)
{
	gdk_threads_enter();
	double done=0.0;
	if(dlnow!=0&&dltotal!=0)done=(double)dlnow/dltotal;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(geany->main_widgets->progressbar), done);
	gchar *doneper = g_strdup_printf("%.2f%%", done*100);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(geany->main_widgets->progressbar), doneper);
	g_free(doneper);
	gdk_threads_leave();
	return 0;
}

static void msgwin_scroll_to_bottom()
{
	GtkTreeView *MsgWin;
	MsgWin = GTK_TREE_VIEW(ui_lookup_widget(geany->main_widgets->window, "treeview4"));
	int n = gtk_tree_model_iter_n_children(gtk_tree_view_get_model(MsgWin),NULL);
	GtkTreePath *path;
	path = gtk_tree_path_new_from_string(g_strdup_printf("%d",n-1));
	gtk_tree_view_scroll_to_cell(MsgWin, path, NULL, FALSE, 0.0, 0.0);
}

static void log_new_str(int color, char *text)
{
	GTimeVal ct;
	g_get_current_time(&ct);
	time_t tm1, tm2;
	struct tm *t1, *t2;
	long sec = 0;
	tm1 = time(NULL);
	t2 = gmtime(&tm1);
	tm2 = mktime(t2);
	t1 = localtime(&tm1);
	sec = ct.tv_sec + (long)(tm1 -tm2);
	msgwin_msg_add(color, -1, NULL, "[%02ld:%02ld:%02ld.%03.0f] %s", 
	(sec/3600)%24, (sec/60)%60, (sec)%60, (double)(ct.tv_usec)/ 1000, text);
	msgwin_scroll_to_bottom();
}

static int ftp_log(CURL *handle, curl_infotype type, char *data, size_t size, void *userp)
{
	char * odata;
	odata = g_strstrip(g_strdup_printf("%s", data));
	char * firstline;
	firstline = strtok(odata,"\r\n");
	gdk_threads_enter();
	switch (type) {
		case CURLINFO_TEXT:
			log_new_str(COLOR_BLUE, firstline);
			break;
		default:
			break;
		case CURLINFO_HEADER_OUT:
			log_new_str(COLOR_BLUE, firstline);
			break;
		case CURLINFO_DATA_OUT:
			break;
		case CURLINFO_SSL_DATA_OUT:
			break;
		case CURLINFO_HEADER_IN:
			log_new_str(COLOR_BLACK, firstline);
			break;
		case CURLINFO_DATA_IN:
			break;
		case CURLINFO_SSL_DATA_IN:
			break;
	}
	gdk_threads_leave();
	return 0;
}

static gboolean is_single_selection(GtkTreeSelection *treesel)
{
	if (gtk_tree_selection_count_selected_rows(treesel) == 1)
		return TRUE;

	ui_set_statusbar(FALSE, _("Too many items selected!"));
	return FALSE;
}

static gboolean is_folder_selected(GList *selected_items)
{
	GList *item;
	GtkTreeModel *model = GTK_TREE_MODEL(file_store);
	gboolean dir_found = FALSE;
	for (item = selected_items; item != NULL; item = g_list_next(item)) {
		gchar *icon;
		GtkTreeIter iter;
		GtkTreePath *treepath;
		treepath = (GtkTreePath*) item->data;
		gtk_tree_model_get_iter(model, &iter, treepath);
		gtk_tree_model_get(model, &iter, FILEVIEW_COLUMN_ICON, &icon, -1);
		if (utils_str_equal(icon, GTK_STOCK_DIRECTORY))	{
			dir_found = TRUE;
			g_free(icon);
			break;
		}
		g_free(icon);
	}
	return dir_found;
}

static int add_item(gpointer data, gboolean is_dir)
{
	gchar **parts=g_regex_split_simple("\n", data, 0, 0);
	if (strcmp(parts[0],".")==0||strcmp(parts[0],"..")==0) return 1;
	GtkTreeIter iter;
	gtk_tree_store_append(file_store, &iter, gtk_tree_store_iter_is_valid(file_store, &parent)?&parent:NULL);
	if (is_dir) {
		GRegex *regex;
		regex = g_regex_new("\\s->\\s", 0, 0, NULL);
		if (g_regex_match(regex, parts[0], 0, NULL)) {
			gchar **nameparts=g_regex_split(regex, parts[0], 0);
			if (g_strcmp0(parts[1],g_strdup_printf("%ld",g_utf8_strlen(nameparts[1],-1)))==0) {
				sprintf(parts[0], "%s", nameparts[0]);
				sprintf(parts[1], "link");
			}
			g_strfreev(nameparts);
		}
		if (strcmp(parts[1],"0")==0) {
			sprintf(parts[1], "%s", "");
		}
	}
	gtk_tree_store_set(file_store, &iter,
	FILEVIEW_COLUMN_ICON, is_dir?GTK_STOCK_DIRECTORY:GTK_STOCK_FILE,
	FILEVIEW_COLUMN_NAME, parts[0],
	FILEVIEW_COLUMN_SIZE, parts[1],
	FILEVIEW_COLUMN_FILENAME, g_strconcat(current_url, parts[0], is_dir?"/":"", NULL),
	-1);
	g_strfreev(parts);
	return 0;
}

static int file_cmp(gconstpointer a, gconstpointer b)
{
	return g_ascii_strncasecmp(a, b, -1);
}

static void clear_children()
{
	gdk_threads_enter();
	GtkTreeIter child;
	if (gtk_tree_store_iter_is_valid(file_store, &parent) && gtk_tree_model_iter_children(GTK_TREE_MODEL(file_store), &child, &parent)) {
		while (gtk_tree_store_remove(file_store, &child)) {}
	}
	gdk_threads_leave();
}

static void clear()
{
	gtk_tree_store_clear(file_store);
}

static int to_list(const char *listdata)
{
	if (strlen(listdata)==0) return 1;
	char * odata;
	odata = g_strdup_printf("%s", listdata);
	char *pch;
	pch = strtok(odata, "\r\n");
	struct ftpparse ftp;
	while (pch != NULL)
	{
		if (ftp_parse(&ftp, pch, strlen(pch))) {
			char *fileinfo;
			fileinfo = g_strdup_printf("%s\n%ld\n%lu", ftp.name, ftp.size, (unsigned long)ftp.mtime);
			switch (ftp.flagtrycwd){
				case 1:
					dirlist = g_list_prepend(dirlist, fileinfo);
					break;
				default:
					filelist = g_list_prepend(filelist, fileinfo);
			}
		}
		pch = strtok(NULL, "\r\n");
	}
	dirlist = g_list_sort(dirlist, (GCompareFunc)file_cmp);
	filelist = g_list_sort(filelist, (GCompareFunc)file_cmp);
	g_list_foreach(dirlist, (GFunc)add_item, (gpointer)TRUE);
	g_list_foreach(filelist, (GFunc)add_item, (gpointer)FALSE);
	g_list_free(dirlist);
	g_list_free(filelist);
	dirlist=NULL;
	filelist=NULL;
	if (gtk_tree_store_iter_is_valid(file_store, &parent))
		gtk_tree_view_expand_row(GTK_TREE_VIEW(file_view), gtk_tree_model_get_path(GTK_TREE_MODEL(file_store), &parent), FALSE);
	return 0;
}

static void *disconnect(gpointer p)
{
	if (curl) {
		log_new_str(COLOR_RED, "Disconnected.");
		curl = NULL;
	}
	clear();
	return NULL;
}

static void *download_file(gpointer p)
{
	const char *url = (const char *)p;
	char *filepath;
	if (curl) {
		FILE *fp;
		char *filedir = g_strdup_printf("%s/gFTP/",(char *)g_get_tmp_dir());
		g_mkdir_with_parents(filedir, 0777);
		filepath = g_strdup_printf("%s%s", filedir, g_path_get_basename(url));
		fp=fopen(filepath,"wb");
		curl_easy_setopt(curl, CURLOPT_URL, url);
		//curl_easy_setopt(curl, CURLOPT_USERPWD, "");
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
		curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, download_progress);
		curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, NULL);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		//curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L);
		curl_easy_perform(curl);
		fclose(fp);
	}
	gdk_threads_enter();
	if (filepath) document_open_file(filepath, FALSE, NULL, NULL);
	gtk_widget_set_sensitive(GTK_WIDGET(box), TRUE);
	gtk_widget_hide(geany->main_widgets->progressbar);
	gdk_threads_leave();
	g_thread_exit(NULL);
	return NULL;
}

static void *get_dir_listing(gpointer p)
{
	clear_children();
	const char *url = (const char *)p;
	struct string str;
	str.len = 0;
	str.ptr = malloc(1);
	str.ptr[0] = '\0';
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		//curl_easy_setopt(curl, CURLOPT_USERPWD, "");
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_function);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &str);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, ftp_log);
		curl_easy_setopt(curl, CURLOPT_DEBUGDATA, NULL);
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		//curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L);
		curl_easy_perform(curl);
	}
	if (to_list(str.ptr)==0) {
		gtk_tool_button_set_stock_id(GTK_TOOL_BUTTON(btn_connect), GTK_STOCK_DISCONNECT);
	} else {
		gdk_threads_enter();
		dialogs_show_msgbox(GTK_MESSAGE_INFO, "This is an empty directory.");
		gdk_threads_leave();
	}
	free(str.ptr);
	gdk_threads_enter();
	gtk_widget_set_sensitive(GTK_WIDGET(box), TRUE);
	ui_progress_bar_stop();
	gdk_threads_leave();
	g_thread_exit(NULL);
	return NULL;
}

static void *to_download_file(gpointer p)
{
	gtk_widget_set_sensitive(GTK_WIDGET(box), FALSE);
	gtk_widget_show(geany->main_widgets->progressbar);
	g_thread_create(&download_file, (gpointer)p, FALSE, NULL);
	return NULL;
}

static void *to_get_dir_listing(gpointer p)
{
	gtk_widget_set_sensitive(GTK_WIDGET(box), FALSE);
	ui_progress_bar_start("Please wait...");
	g_thread_create(&get_dir_listing, (gpointer)p, FALSE, NULL);
	return NULL;
}

static void on_open_clicked(GtkMenuItem *menuitem, gpointer user_data)
{
	GtkTreeSelection *treesel;
	GtkTreeModel *model = GTK_TREE_MODEL(file_store);
	GList *list;
	treesel = gtk_tree_view_get_selection(GTK_TREE_VIEW(file_view));
	list = gtk_tree_selection_get_selected_rows(treesel, &model);
	
	if (is_single_selection(treesel)) {
		GtkTreePath *treepath = list->data;
		GtkTreeIter iter;
		gtk_tree_model_get_iter(model, &iter, treepath);
		gchar *name;
		gtk_tree_model_get(model, &iter, FILEVIEW_COLUMN_FILENAME, &name, -1);
		current_url=g_strdup_printf("%s", name);
		if (is_folder_selected(list)) {
			gtk_tree_model_get_iter(model, &parent, treepath);
			gtk_entry_set_text(GTK_ENTRY(url_entry), current_url);
			to_get_dir_listing(current_url);
		} else {
			to_download_file(current_url);
		}
		g_free(name);
	}
	g_list_foreach(list, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(list);
	
}

static void on_connect_clicked(gpointer p)
{
	if (!curl) {
		current_url=g_strdup_printf("%s", gtk_entry_get_text(GTK_ENTRY(url_entry)));
		current_url=g_strstrip(current_url);
		if (!g_regex_match_simple("^ftp://", current_url, G_REGEX_CASELESS, 0)) {
			current_url=g_strconcat("ftp://", current_url, NULL);
		}
		if (!g_str_has_suffix(current_url, "/")) {
			current_url=g_strconcat(current_url, "/", NULL);
		}
		gtk_entry_set_text(GTK_ENTRY(url_entry), current_url);
		
		gtk_paned_set_position(GTK_PANED(ui_lookup_widget(geany->main_widgets->window, "vpaned1")), 
			geany->main_widgets->window->allocation.height - 250);
		
		msgwin_clear_tab(MSG_MESSAGE);
		msgwin_switch_tab(MSG_MESSAGE, TRUE);
		log_new_str(COLOR_BLUE, "Connecting...");
		
		GtkTreeIter iter;
		gtk_tree_store_append(file_store, &iter, NULL);
		gtk_tree_store_set(file_store, &iter,
		FILEVIEW_COLUMN_ICON, GTK_STOCK_DIRECTORY,
		FILEVIEW_COLUMN_NAME, current_url,
		FILEVIEW_COLUMN_SIZE, "-1",
		FILEVIEW_COLUMN_FILENAME, current_url,
		-1);
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(file_view), gtk_tree_model_get_path(GTK_TREE_MODEL(file_store), &iter), NULL, FALSE);
		
		curl = curl_easy_init();
		
		on_open_clicked(NULL, NULL);
		
	} else {
		disconnect(NULL);
		gtk_tool_button_set_stock_id(GTK_TOOL_BUTTON(btn_connect), GTK_STOCK_CONNECT);
	}
}

static GtkWidget *create_popup_menu(void)
{
	GtkWidget *item, *menu;
	
	menu = gtk_menu_new();
	
	item = gtk_image_menu_item_new_from_stock(GTK_STOCK_OPEN, NULL);
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_open_clicked), NULL);
	
	return menu;
}

static void load_profiles(gboolean fill_combo_box)
{
	GKeyFile *profiles = g_key_file_new();
	profiles_file = g_strconcat(geany->app->configdir, G_DIR_SEPARATOR_S, "plugins", G_DIR_SEPARATOR_S, "gFTP", G_DIR_SEPARATOR_S, "profiles.conf", NULL);
	g_key_file_load_from_file(profiles, profiles_file, G_KEY_FILE_NONE, NULL);
	all_profiles = g_key_file_get_groups(profiles, &all_profiles_length);
	if (fill_combo_box) {
		GtkTreeIter iter;
		gsize i;
		for (i = 0; i < all_profiles_length; i++) {
			gtk_list_store_append(GTK_LIST_STORE(pref.store), &iter);
			gtk_list_store_set(GTK_LIST_STORE(pref.store), &iter, 
			0, utils_get_setting_string(profiles, all_profiles[i], "host", ""), 
			1, utils_get_setting_string(profiles, all_profiles[i], "port", "21"), 
			2, utils_get_setting_string(profiles, all_profiles[i], "login", ""), 
			3, utils_get_setting_string(profiles, all_profiles[i], "password", ""), 
			-1);
			g_free(all_profiles[i]);
		}
	}
	g_key_file_free(profiles);
}

static void load_settings(void)
{
	load_profiles(FALSE);
}

static void save_profiles(void)
{
	GKeyFile *profiles = g_key_file_new();
	gchar *data;
	gchar *profiles_dir = g_path_get_dirname(profiles_file);
	GtkTreeModel *model;
	model = gtk_combo_box_get_model(GTK_COMBO_BOX(pref.combo));
	GtkTreeIter iter;
	gboolean valid;
	valid = gtk_tree_model_get_iter_from_string(model, &iter, "2");
	gchar *host = NULL;
	gchar *port = NULL;
	gchar *login = NULL;
	gchar *password = NULL;
	while (valid) {
		gtk_tree_model_get(GTK_TREE_MODEL(pref.store), &iter, 
		0, &host, 
		1, &port, 
		2, &login, 
		3, &password, 
		-1);
		if (g_strcmp0(host, "")!=0) {
			host = g_strstrip(host);
			g_key_file_set_string(profiles, host, "host", host);
			g_key_file_set_string(profiles, host, "port", g_strstrip(port));
			g_key_file_set_string(profiles, host, "login", g_strstrip(login));
			g_key_file_set_string(profiles, host, "password", g_strstrip(password));
			g_free(host);
			g_free(port);
			g_free(login);
			g_free(password);
		}
		valid = gtk_tree_model_iter_next(model, &iter);
	}
	if (!g_file_test(profiles_dir, G_FILE_TEST_IS_DIR) && utils_mkdir(profiles_dir, TRUE)!=0) {
		dialogs_show_msgbox(GTK_MESSAGE_ERROR, "Plugin configuration directory could not be created.");
	} else {
		data = g_key_file_to_data(profiles, NULL, NULL);
		utils_write_file(profiles_file, data);
		g_free(data);
	}
	g_free(profiles_dir);
	g_key_file_free(profiles);
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	if (event->button == 1 && event->type == GDK_2BUTTON_PRESS) {
		on_open_clicked(NULL, NULL);
		return TRUE;
	} else if (event->button == 3) {
		static GtkWidget *popup_menu = NULL;
		if (popup_menu==NULL) popup_menu = create_popup_menu();
		gtk_menu_popup(GTK_MENU(popup_menu), NULL, NULL, NULL, NULL, event->button, event->time);
	}
	return FALSE;
}

static void on_edit_preferences(void)
{
	plugin_show_configure(geany_plugin);
}

static gboolean is_edit_profiles_selected_nth_item(GtkTreeIter *iter, char *num)
{
	return gtk_tree_path_compare(gtk_tree_path_new_from_string(gtk_tree_model_get_string_from_iter(GTK_TREE_MODEL(pref.store), iter)), gtk_tree_path_new_from_string(num))==0;
}

static void is_select_profiles_use_anonymous(GtkTreeIter *iter)
{
	gboolean toggle = FALSE;
	gchar *login = g_strdup_printf("%s", "");
	if (gtk_list_store_iter_is_valid(GTK_LIST_STORE(pref.store), iter)) {
		gtk_tree_model_get(GTK_TREE_MODEL(pref.store), iter, 2, &login, -1);
		if (g_strcmp0(login, "anonymous")==0) {
			toggle = TRUE;
		}
	}
	g_free(login);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pref.anon), toggle);
	gtk_widget_set_sensitive(pref.login, !toggle);
	gtk_widget_set_sensitive(pref.passwd, !toggle);
}

static void check_delete_button_sensitive(GtkTreeIter *iter)
{
	gtk_widget_set_sensitive(pref.delete, !is_edit_profiles_selected_nth_item(iter, "0"));
}

static void *on_host_login_password_changed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	if (widget) {
		if (!gtk_list_store_iter_is_valid(GTK_LIST_STORE(pref.store), &pref.iter_store_new) || is_edit_profiles_selected_nth_item(&pref.iter_store_new, "0")) {
			gtk_list_store_append(GTK_LIST_STORE(pref.store), &pref.iter_store_new);
		}
		gtk_list_store_set(GTK_LIST_STORE(pref.store), &pref.iter_store_new, 
		0, gtk_entry_get_text(GTK_ENTRY(pref.host)), 
		1, gtk_entry_get_text(GTK_ENTRY(pref.port)), 
		2, gtk_entry_get_text(GTK_ENTRY(pref.login)), 
		3, gtk_entry_get_text(GTK_ENTRY(pref.passwd)), 
		-1);
		gtk_combo_box_set_active_iter(GTK_COMBO_BOX(pref.combo), &pref.iter_store_new);
	}
	return FALSE;
}

static void on_use_anonymous_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
	gboolean toggle = gtk_toggle_button_get_active(togglebutton);
	gtk_widget_set_sensitive(pref.login, !toggle);
	gtk_widget_set_sensitive(pref.passwd, !toggle);
	if (toggle){
		gtk_entry_set_text(GTK_ENTRY(pref.login), "anonymous");
		gtk_entry_set_text(GTK_ENTRY(pref.passwd), "ftp@example.com");
	}
	on_host_login_password_changed(NULL, NULL, NULL);
}

static void on_show_password_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
	gtk_entry_set_visibility(GTK_ENTRY(pref.passwd), gtk_toggle_button_get_active(togglebutton));
}

static void on_edit_profiles_changed(void)
{
	GtkTreeIter iter;
	gtk_combo_box_get_active_iter(GTK_COMBO_BOX(pref.combo), &iter);
	gtk_combo_box_get_active_iter(GTK_COMBO_BOX(pref.combo), &pref.iter_store_new);
	gchar *host = g_strdup_printf("%s", "");
	gchar *port = g_strdup_printf("%s", "21");
	gchar *login = g_strdup_printf("%s", "");
	gchar *password = g_strdup_printf("%s", "");
	if (!is_edit_profiles_selected_nth_item(&iter, "0")) {
		if (gtk_list_store_iter_is_valid(GTK_LIST_STORE(pref.store), &iter)) {
			gtk_tree_model_get(GTK_TREE_MODEL(pref.store), &iter, 
			0, &host, 
			1, &port, 
			2, &login, 
			3, &password, 
			-1);
		}
	}
	gtk_entry_set_text(GTK_ENTRY(pref.host), host);
	gtk_entry_set_text(GTK_ENTRY(pref.port), port);
	gtk_entry_set_text(GTK_ENTRY(pref.login), login);
	gtk_entry_set_text(GTK_ENTRY(pref.passwd), password);
	g_free(host);
	g_free(port);
	g_free(login);
	g_free(password);
	
	is_select_profiles_use_anonymous(&iter);
	check_delete_button_sensitive(&iter);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pref.showpass), FALSE);
}

static void on_delete_profile_clicked()
{
	GtkTreeIter iter;
	gtk_combo_box_get_active_iter(GTK_COMBO_BOX(pref.combo), &iter);
	if (!is_edit_profiles_selected_nth_item(&iter, "0")) {
		gtk_list_store_remove(GTK_LIST_STORE(pref.store), &iter);
		int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(pref.store),NULL);
		if (n==2) n=1;
		gtk_combo_box_set_active(GTK_COMBO_BOX(pref.combo), n-1);
	}
}

static gboolean profiles_treeview_row_is_separator(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	return is_edit_profiles_selected_nth_item(iter, "1");
}

static void on_configure_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY)
	{
		save_profiles();
	}
}

static void prepare_file_view()
{
	file_store = gtk_tree_store_new(FILEVIEW_N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	gtk_tree_view_set_model(GTK_TREE_VIEW(file_view), GTK_TREE_MODEL(file_store));
	g_object_unref(file_store);
	
	GtkCellRenderer *text_renderer, *icon_renderer;
	GtkTreeViewColumn *column;
	column = gtk_tree_view_column_new();
	icon_renderer = gtk_cell_renderer_pixbuf_new();
	gtk_tree_view_column_pack_start(column, icon_renderer, FALSE);
	gtk_tree_view_column_set_attributes(column, icon_renderer, "stock-id", FILEVIEW_COLUMN_ICON, NULL);
	text_renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(column, text_renderer, TRUE);
	gtk_tree_view_column_set_attributes(column, text_renderer, "text", FILEVIEW_COLUMN_NAME, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(file_view), column);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(file_view), FALSE);
	
	PangoFontDescription *pfd = pango_font_description_new();
	pango_font_description_set_size(pfd, 8 * PANGO_SCALE);
	gtk_widget_modify_font(file_view, pfd);
	
	gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(file_view), FILEVIEW_COLUMN_FILENAME);
	
	g_signal_connect(file_view, "button-press-event", G_CALLBACK(on_button_press), NULL);
}

static GtkWidget *make_toolbar(void)
{
	GtkWidget *wid, *toolbar;
	
	toolbar = gtk_toolbar_new();
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_MENU);
	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);
	
	btn_connect = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_CONNECT));
	gtk_widget_set_tooltip_text(btn_connect, "Connect / Disconnect");
	//gtk_widget_set_sensitive(btn_connect, FALSE);
	g_signal_connect(btn_connect, "clicked", G_CALLBACK(on_connect_clicked), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), btn_connect);
		
	wid = GTK_WIDGET(gtk_tool_button_new_from_stock(GTK_STOCK_PREFERENCES));
	gtk_widget_set_tooltip_text(wid, "Preferences");
	g_signal_connect(wid, "clicked", G_CALLBACK(on_edit_preferences), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), wid);
	
	return toolbar;
}

void plugin_init(GeanyData *data)
{
	curl_global_init(CURL_GLOBAL_ALL);
	
	if (!g_thread_supported()) g_thread_init(NULL);
	
	gdk_threads_init();
	
	gdk_threads_enter();
	
	box = gtk_vbox_new(FALSE, 0);
	
	url_entry = gtk_entry_new_with_buffer(gtk_entry_buffer_new("137.189.4.14",-1));
	gtk_box_pack_start(GTK_BOX(box), url_entry, FALSE, FALSE, 0);
	
	GtkWidget *widget;
	
	widget = make_toolbar();
	gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
	
	file_view = gtk_tree_view_new();
	prepare_file_view();
	
	widget = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(widget), file_view);
	gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
	
	load_settings();
	
	gtk_widget_show_all(box);
	gtk_notebook_append_page(GTK_NOTEBOOK(geany->main_widgets->sidebar_notebook), box, gtk_label_new(_("FTP")));
	gdk_threads_leave();
}

GtkWidget *plugin_configure(GtkDialog *dialog)
{
	GtkWidget *widget, *vbox, *table;
	
	vbox = gtk_vbox_new(FALSE, 6);
	
	widget = gtk_label_new("<b>Profiles</b>");
	gtk_label_set_use_markup(GTK_LABEL(widget), TRUE);
	gtk_misc_set_alignment(GTK_MISC(widget), 0, 0.5);
	gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 0);
	
	GtkListStore *store;
	store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	GtkTreeIter iter;
	gtk_list_store_append(store, &iter);
	gtk_list_store_set(store, &iter, 0, "New profile...", -1);
	widget = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
	gtk_list_store_append(store, &iter); //for separator
	pref.store = store;
	pref.combo = widget;
	g_object_unref(G_OBJECT(store));
	
	GtkCellRenderer *renderer;
	renderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(widget), renderer, TRUE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(widget), renderer, "text", 0);
	gtk_combo_box_set_active(GTK_COMBO_BOX(widget), 0);
	gtk_widget_set_tooltip_text(widget, "Choose a profile to edit or choose New profile to create one.");
	gtk_combo_box_set_row_separator_func(GTK_COMBO_BOX(widget), (GtkTreeViewRowSeparatorFunc)profiles_treeview_row_is_separator, NULL, NULL);
	g_signal_connect(widget, "changed", G_CALLBACK(on_edit_profiles_changed), NULL);
	
	load_profiles(TRUE);
	
	table = gtk_table_new(4, 4, FALSE);
	
	gtk_table_attach(GTK_TABLE(table), widget, 0, 2, 0, 1, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	widget = gtk_button_new_from_stock(GTK_STOCK_DELETE);
	gtk_table_attach(GTK_TABLE(table), widget, 2, 4, 0, 1, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	g_signal_connect(widget, "clicked", G_CALLBACK(on_delete_profile_clicked), NULL);
	pref.delete = widget;
	check_delete_button_sensitive(NULL);
	
	widget = gtk_label_new("Host");
	gtk_misc_set_alignment(GTK_MISC(widget), 1, 0.5);
	gtk_table_attach(GTK_TABLE(table), widget, 0, 1, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	widget = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), widget, 1, 2, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	g_signal_connect(widget, "key-release-event", G_CALLBACK(on_host_login_password_changed), NULL);
	pref.host = widget;
	
	widget = gtk_label_new("Port");
	gtk_table_attach(GTK_TABLE(table), widget, 2, 3, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	widget = gtk_entry_new();
	gtk_widget_set_size_request(widget, 40, -1);
	gtk_entry_set_text(GTK_ENTRY(widget), "21");
	gtk_table_attach(GTK_TABLE(table), widget, 3, 4, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	g_signal_connect(widget, "key-release-event", G_CALLBACK(on_host_login_password_changed), NULL);
	pref.port = widget;
	
	widget = gtk_label_new("Login");
	gtk_misc_set_alignment(GTK_MISC(widget), 1, 0.5);
	gtk_table_attach(GTK_TABLE(table), widget, 0, 1, 2, 3, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	widget = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), widget, 1, 2, 2, 3, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	g_signal_connect(widget, "key-release-event", G_CALLBACK(on_host_login_password_changed), NULL);
	pref.login = widget;
	widget = gtk_check_button_new_with_label("Anonymous");
	gtk_table_attach(GTK_TABLE(table), widget, 2, 4, 2, 3, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	g_signal_connect(widget, "toggled", G_CALLBACK(on_use_anonymous_toggled), NULL);
	pref.anon = widget;
	
	widget = gtk_label_new("Password");
	gtk_misc_set_alignment(GTK_MISC(widget), 1, 0.5);
	gtk_table_attach(GTK_TABLE(table), widget, 0, 1, 3, 4, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	widget = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(widget), FALSE);
	gtk_table_attach(GTK_TABLE(table), widget, 1, 2, 3, 4, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	g_signal_connect(widget, "key-release-event", G_CALLBACK(on_host_login_password_changed), NULL);
	pref.passwd = widget;
	widget = gtk_check_button_new_with_label("Show");
	gtk_table_attach(GTK_TABLE(table), widget, 2, 4, 3, 4, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 2, 2);
	g_signal_connect(widget, "toggled", G_CALLBACK(on_show_password_toggled), NULL);
	pref.showpass = widget;
	
	gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, FALSE, 0);
	
	gtk_widget_show_all(vbox);
	
	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), NULL);
	return vbox;
}

void plugin_cleanup(void)
{
	curl_global_cleanup();
	g_free(profiles_file);
	g_strfreev(all_profiles);
	gtk_widget_destroy(box);
}
