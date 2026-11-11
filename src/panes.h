/* Parent listing and cached previews for the three-pane layout. */

#define PANE_PREVIEW_BYTES (128 * 1024)

static struct {
	char path[PATH_MAX];
	struct stat info;
	struct dirent **entries;
	int count;
} parent_pane;

static struct {
	char path[PATH_MAX];
	struct stat info;
	int width, height;
	bool showhidden;
	size_t length;
	unsigned char data[PANE_PREVIEW_BYTES + 1];
} file_preview;

static bool same_pane_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino
	       && a->st_size == b->st_size && a->st_mtime == b->st_mtime
	       && a->st_ctime == b->st_ctime && NSEC_MTIME(*a) == NSEC_MTIME(*b)
	       && NSEC_CTIME(*a) == NSEC_CTIME(*b);
}

static void pane_text(const char *text, size_t length, int columns)
{
#ifndef NOLC
	mbstate_t state = {0};
#endif
	while (length && columns > 0) {
#ifdef NOLC
		unsigned char ch = (unsigned char)*text;
		addch(ch == '\t' ? ' ' : (ch < 32 || ch == 127 ? '?' : ch));
		++text;
		--length;
		--columns;
#else
		wchar_t ch;
		size_t bytes = mbrtowc(&ch, text, length, &state);
		if (!bytes || bytes == (size_t)-1 || bytes == (size_t)-2) {
			ch = L'?';
			bytes = 1;
			memset(&state, 0, sizeof(state));
		}
		int width = wcwidth(ch);
		if (width < 0) {
			ch = ch == L'\t' ? L' ' : L'?';
			width = 1;
		}
		if (width > columns)
			break;
		addnwstr(&ch, 1);
		text += bytes;
		length -= bytes;
		columns -= width;
#endif
	}
}

static void pane_title(int column, int width, const char *title)
{
	move(0, column);
	attron(A_BOLD | COLOR_PAIR(cfg.curctx + 1));
	pane_text(title, strlen(title), width);
	attroff(A_BOLD | COLOR_PAIR(cfg.curctx + 1));
}

static void pane_border(int column)
{
	mvvline(0, column, ACS_VLINE | A_DIM | COLOR_PAIR(cfg.curctx + 1), xlines - 1);
}

static int pane_entry(const struct dirent *entry)
{
	return strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
}

static int pane_entry_order(const struct dirent **a, const struct dirent **b)
{
	int directory_order = ((*b)->d_type == DT_DIR) - ((*a)->d_type == DT_DIR);
	return directory_order ? directory_order : xstrverscasecmp((*a)->d_name, (*b)->d_name);
}

static void free_parent_pane(void)
{
	for (int i = 0; i < parent_pane.count; ++i)
		free(parent_pane.entries[i]);
	free(parent_pane.entries);
	parent_pane.entries = NULL;
	parent_pane.count = 0;
	parent_pane.path[0] = '\0';
}

static void draw_parent_pane(char *path)
{
	char parent[PATH_MAX];
	const char *current = xbasename(path);
	xstrsncpy(parent, path, sizeof(parent));
	xdirname(parent);

	pane_border(list_left - 1);
	pane_title(1, list_left - 3, istopdir(parent) ? "/" : xbasename(parent));

	struct stat info;
	if (stat(parent, &info) == -1)
		return;
	if (strcmp(parent_pane.path, parent) || !same_pane_file(&parent_pane.info, &info)) {
		free_parent_pane();
		struct dirent **entries = NULL;
		int count = scandir(parent, &entries, pane_entry, pane_entry_order);
		if (count < 0)
			return;
		parent_pane.entries = entries;
		parent_pane.count = count;
		parent_pane.info = info;
		xstrsncpy(parent_pane.path, parent, sizeof(parent_pane.path));
	}

	int rows = xlines - 3, visible = 0, current_index = 0;
	for (int i = 0; i < parent_pane.count; ++i) {
		const char *name = parent_pane.entries[i]->d_name;
		if (!strcmp(name, current))
			current_index = visible;
		else if (!cfg.showhidden && name[0] == '.')
			continue;
		++visible;
	}

	int first = MAX(0, MIN(current_index - rows / 2, visible - rows));
	for (int i = 0, row = 2; i < parent_pane.count && row < xlines - 1; ++i) {
		const struct dirent *entry = parent_pane.entries[i];
		bool active = !strcmp(entry->d_name, current);
		if ((!active && !cfg.showhidden && entry->d_name[0] == '.') || first-- > 0)
			continue;
		int attrs = active ? A_REVERSE | A_BOLD : A_DIM;
		if (entry->d_type == DT_DIR || active)
			attrs |= COLOR_PAIR(C_DIR);
		move(row++, 1);
		attron(attrs);
#ifdef ICONS_ENABLED
		addstr(entry->d_type == DT_DIR || active ? "\uf07b " : "\uf15b ");
#else
		addstr(entry->d_type == DT_DIR || active ? "/ " : "  ");
#endif
		pane_text(entry->d_name, strlen(entry->d_name), list_left - 5);
		attroff(attrs);
	}
}

static int pane_cube(unsigned char value)
{
	return value < 48 ? 0 : (value < 115 ? 1 : (value - 35) / 40);
}

static bool draw_pane_image(int column, int columns, int rows)
{
	int width, height, offset = 0;
	unsigned char *data = file_preview.data + 1;
	if (sscanf((const char *)data, "P6\n%d %d\n255%n", &width, &height, &offset) != 2
	    || offset <= 0 || data[offset] != '\n' || width < 1 || width > columns
	    || height < 1 || height > rows
	    || (size_t)width * height * 3 + (size_t)offset + 2 > file_preview.length
	    || COLORS < COLOR_256 || COLOR_PAIRS < C_PANE_IMAGE + 216)
		return FALSE;

	data += offset + 1;
	int previous = -1;
	for (int y = 0; y < height; ++y) {
		move(y + 2, column);
		for (int x = 0; x < width; ++x, data += 3) {
			int color = 36 * pane_cube(data[0]) + 6 * pane_cube(data[1]) + pane_cube(data[2]);
			if (color != previous) {
				attr_set(A_NORMAL, C_PANE_IMAGE + color, NULL);
				previous = color;
			}
#ifndef NOLC
			addwstr(L"█");
#else
			addch('#');
#endif
		}
	}
	attr_set(A_NORMAL, 0, NULL);
	return TRUE;
}

static void load_pane_preview(char *path, const struct stat *info, int width, int height)
{
	xstrsncpy(file_preview.path, path, sizeof(file_preview.path));
	file_preview.info = *info;
	file_preview.width = width;
	file_preview.height = height;
	file_preview.showhidden = cfg.showhidden;
	file_preview.length = 0;
	file_preview.data[0] = '\0';

	if (!previewer) {
		previewer = malloc(PATH_MAX);
		if (previewer)
			mkpath(plgpath, utils[UTIL_NPREVIEW], previewer);
	}
	int pipefd[2];
	if (!previewer || access(previewer, X_OK) || pipe(pipefd) == -1)
		return;

	char cols[16], rows[16];
	snprintf(cols, sizeof(cols), "%d", width);
	snprintf(rows, sizeof(rows), "%d", height);
	char *argv[] = {previewer, path, cols, rows, "pane", cfg.showhidden ? "all" : "", NULL};
	pid_t child;
	int status = spawn_redirected(previewer, argv, pipefd[0], pipefd[1], &child);
	close(pipefd[1]);
	if (!status) {
		while (file_preview.length < PANE_PREVIEW_BYTES) {
			ssize_t n = read(pipefd[0], file_preview.data + file_preview.length,
					 PANE_PREVIEW_BYTES - file_preview.length);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			file_preview.length += n;
		}
	}
	close(pipefd[0]);
	if (!status)
		while (waitpid(child, NULL, 0) == -1 && errno == EINTR) {}
	file_preview.data[file_preview.length] = '\0';
}

static void draw_file_pane(const char *path)
{
	int column = preview_border + 2, width = xcols - column - 1, rows = xlines - 3;
	pane_border(preview_border);
	if (!ndents)
		return;

	const char *name = pdents[cur].name;
	pane_title(column, width, name);
	char target[PATH_MAX];
	struct stat info;
	mkpath(path, name, target);
	if (stat(target, &info) == -1 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
		return;
	if (strcmp(file_preview.path, target) || file_preview.width != width
	    || file_preview.height != rows || file_preview.showhidden != cfg.showhidden
	    || !same_pane_file(&file_preview.info, &info))
		load_pane_preview(target, &info, width, rows);
	if (!file_preview.length)
		return;
	if (!file_preview.data[0]) {
		if (!draw_pane_image(column, width, rows))
			mvaddnstr(2, column, "Image preview unavailable", width);
		return;
	}

	const char *line = (const char *)file_preview.data;
	const char *end = line + file_preview.length;
	for (int row = 2; row < xlines - 1 && line < end; ++row) {
		const char *next = memchr(line, '\n', end - line);
		const char *stop = next ? next : end;
		if (stop > line && stop[-1] == '\r')
			--stop;
		move(row, column);
		pane_text(line, stop - line, width);
		line = next ? next + 1 : end;
	}
}
