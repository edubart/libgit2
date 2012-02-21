#include "ignore.h"
	git_status_t   status,
	mode_t         attr,
	const char    *path)
		if (oid != NULL)
			git_oid_cpy(&delta->new_oid, oid);
		if (oid != NULL)
			git_oid_cpy(&delta->old_oid, oid);
static int create_diff_for_tree_entry(const char *root, git_tree_entry *entry, void *data)
		diff, diff->status, git_tree_entry_attributes(entry),
static int tree_to_tree_diff_cb(const git_tree_diff_data *tdiff, void *data)
			error = git_tree_diff(old, new, tree_to_tree_diff_cb, diff);
		diff->status       = added_dir ? GIT_STATUS_ADDED : GIT_STATUS_DELETED;
			error = git_tree_walk(
				tree, create_diff_for_tree_entry, GIT_TREEWALK_POST, diff);
	error = git_tree_diff(old, new, tree_to_tree_diff_cb, diff);
	if (error == GIT_SUCCESS) {
	git_index     *index;
	unsigned int  index_pos;
	git_ignores   *ignores;
} diff_callback_info;
	diff_callback_info *info,
	git_status_t status,
			info->diff, status, idx_entry->mode,
	diff_callback_info *info = data;
	error = add_new_index_deltas(info, GIT_STATUS_ADDED, info->diff->pfx.ptr);
	diff_callback_info info = {0};
			error = add_new_index_deltas(&info, GIT_STATUS_ADDED, NULL);
typedef struct {
	struct stat st;
	mode_t mode;
	char path[GIT_FLEX_ARRAY];
} workdir_entry;

#define MODE_PERMS_MASK 0777

/* TODO: need equiv of core git's "trust_executable_bit" flag? */
#define CANONICAL_PERMS(MODE) (((MODE) & 0100) ? 0755 : 0644)
#define MODE_TYPE(MODE)  ((MODE) & ~MODE_PERMS_MASK)

static mode_t canonical_mode(mode_t raw_mode)
{
	if (S_ISREG(raw_mode))
		return S_IFREG | CANONICAL_PERMS(raw_mode);
	else if (S_ISLNK(raw_mode))
		return S_IFLNK;
	else if (S_ISDIR(raw_mode))
		return S_IFDIR;
	else if (S_ISGITLINK(raw_mode))
		return S_IFGITLINK;
	else
		return 0;
}

static int diff_workdir_insert(void *data, git_buf *dir)
{
	workdir_entry *wd_entry = git__malloc(sizeof(workdir_entry) + dir->size + 2);
	if (wd_entry == NULL)
		return GIT_ENOMEM;
	if (p_lstat(dir->ptr, &wd_entry->st) < 0) {
		git__free(wd_entry);
		return GIT_EOSERR;
	}
	git_buf_copy_cstr(wd_entry->path, dir->size + 1, dir);
	wd_entry->mode = canonical_mode(wd_entry->st.st_mode);
	/* suffix directories with / to mimic tree/index sort order */
	if (S_ISDIR(wd_entry->st.st_mode)) {
		wd_entry->path[dir->size] = '/';
		wd_entry->path[dir->size+1] = '\0';
	}

	return git_vector_insert((git_vector *)data, wd_entry);
}

static int diff_workdir_walk(
	const char *dir,
	diff_callback_info *info,
	int (*cb)(diff_callback_info *, workdir_entry *))
{
	int error = GIT_SUCCESS;
	git_vector files = GIT_VECTOR_INIT;
	git_buf buf = GIT_BUF_INIT;
	unsigned int i;
	workdir_entry *wd_entry;
	git_ignores ignores = {0}, *old_ignores = info->ignores;

	if (!dir)
		dir = git_repository_workdir(info->diff->repo);

	if ((error = git_vector_init(&files, 0, git__strcmp_cb)) < GIT_SUCCESS ||
		(error = git_buf_sets(&buf, dir)) < GIT_SUCCESS ||
		(error = git_path_direach(&buf, diff_workdir_insert, &files)) < GIT_SUCCESS ||
		(error = git_ignore__for_path(info->diff->repo, dir, &ignores)) < GIT_SUCCESS)
		goto cleanup;

	git_vector_sort(&files);
	info->ignores = old_ignores;

	git_vector_foreach(&files, i, wd_entry) {
		if ((error = cb(info, wd_entry)) < GIT_SUCCESS)
			goto cleanup;
	}

cleanup:
	git_vector_foreach(&files, i, wd_entry)
		git__free(wd_entry);
	info->ignores = old_ignores;
	git_ignore__free(&ignores);
	git_vector_free(&files);
	git_buf_free(&buf);

	return error;
}

static int found_new_workdir_entry(
	diff_callback_info *info, workdir_entry *wd_entry)
{
	int error;
	int ignored = 0;
	git_status_t status;

	/* skip file types that are not trackable */
	if (wd_entry->mode == 0)
		return GIT_SUCCESS;

	error = git_ignore__lookup(info->ignores, wd_entry->path, &ignored);
	if (error < GIT_SUCCESS)
		return error;
	status = ignored ? GIT_STATUS_IGNORED : GIT_STATUS_UNTRACKED;

	return file_delta_new__from_one(
		info->diff, status, wd_entry->mode, NULL, wd_entry->path);
}

static int diff_workdir_to_index_cb(
	diff_callback_info *info, workdir_entry *wd_entry)
{
	int error, modified;
	git_index_entry *idx_entry;
	git_oid new_oid;

	/* Store index entries that precede this workdir entry */
	error = add_new_index_deltas(info, GIT_STATUS_DELETED, wd_entry->path);
	if (error < GIT_SUCCESS)
		return error;

	/* Process workdir entries that are not in the index.
	 * These might be untracked, ignored, or special (dirs, etc).
	 */
	idx_entry = git_index_get(info->index, info->index_pos);
	if (idx_entry == NULL || strcmp(idx_entry->path, wd_entry->path) > 0) {
		git_buf dotgit = GIT_BUF_INIT;
		int contains_dotgit;

		if (!S_ISDIR(wd_entry->mode))
			return found_new_workdir_entry(info, wd_entry);

		error = git_buf_joinpath(&dotgit, wd_entry->path, DOT_GIT);
		if (error < GIT_SUCCESS)
			return error;
		contains_dotgit = (git_path_exists(dotgit.ptr) == GIT_SUCCESS);
		git_buf_free(&dotgit);

		if (contains_dotgit)
			/* TODO: deal with submodule or embedded repo */
			return GIT_SUCCESS;
		else if (git__prefixcmp(idx_entry->path, wd_entry->path) == GIT_SUCCESS)
			/* there are entries in the directory in the index already,
			 * so recurse into it.
			 */
			return diff_workdir_walk(wd_entry->path, info, diff_workdir_to_index_cb);
		else
			/* TODO: this is not the same behavior as core git.
			 *
			 * I don't recurse into the directory once I know that no files
			 * in it are being tracked.  But core git does and only adds an
			 * entry if there are non-directory entries contained under the
			 * dir (although, interestingly, it only shows the dir, not the
			 * individual entries).
			 */
			return found_new_workdir_entry(info, wd_entry);
	}

	/* create modified delta for non-matching tree & index entries */
	info->index_pos++;

	/* check for symlink/blob changes and split into add/del pair */
	if (MODE_TYPE(wd_entry->mode) != MODE_TYPE(idx_entry->mode)) {
		error = file_delta_new__from_one(
			info->diff, GIT_STATUS_DELETED,
			idx_entry->mode, &idx_entry->oid, idx_entry->path);
		if (error < GIT_SUCCESS)
			return error;

		/* because of trailing slash, cannot have non-dir to dir transform */
		assert(!S_ISDIR(wd_entry->mode));

		return file_delta_new__from_one(
			info->diff, GIT_STATUS_ADDED,
			wd_entry->mode, NULL, wd_entry->path);
	}

	/* mode or size changed, so git blob has definitely changed */
	if (wd_entry->mode != idx_entry->mode ||
		wd_entry->st.st_size != idx_entry->file_size)
	{
		modified = 1;
		memset(&new_oid, 0, sizeof(new_oid));
	}

	/* all other things are indicators there might be a change, so get oid */
	if (!modified &&
		((git_time_t)wd_entry->st.st_ctime != idx_entry->ctime.seconds ||
		 (git_time_t)wd_entry->st.st_mtime != idx_entry->mtime.seconds ||
		 (unsigned int)wd_entry->st.st_dev != idx_entry->dev ||
		 (unsigned int)wd_entry->st.st_ino != idx_entry->ino ||
		 /* TODO: need TRUST_UID_GID configs */
		 (unsigned int)wd_entry->st.st_uid != idx_entry->uid ||
		 (unsigned int)wd_entry->st.st_gid != idx_entry->gid))
	{
		/* calculate oid to confirm change */
		if (S_ISLNK(wd_entry->st.st_mode))
			error = git_odb__hashlink(&new_oid, wd_entry->path);
		else {
			int fd;
			if ((fd = p_open(wd_entry->path, O_RDONLY)) < 0)
				error = git__throw(
					GIT_EOSERR, "Could not open '%s'", wd_entry->path);
			else {
				error = git_odb__hashfd(
					&new_oid, fd, wd_entry->st.st_size, GIT_OBJ_BLOB);
				p_close(fd);
			}
		}

		if (error < GIT_SUCCESS)
			return error;

		modified = (git_oid_cmp(&new_oid, &idx_entry->oid) != 0);
	}

	/* TODO: check index flags for forced ignore changes */

	if (modified) {
		git_tree_diff_data tdiff;

		tdiff.old_attr = idx_entry->mode;
		tdiff.new_attr = wd_entry->mode;
		tdiff.status   = GIT_STATUS_MODIFIED;
		tdiff.path     = wd_entry->path;
		git_oid_cpy(&tdiff.old_oid, &idx_entry->oid);
		git_oid_cpy(&tdiff.new_oid, &new_oid);

		error = file_delta_new__from_tree_diff(info->diff, &tdiff);
	}

	return error;
}

int git_diff_workdir_to_index(
	git_repository *repo,
	const git_diff_options *opts,
	git_diff_list **diff)
{
	int error;
	diff_callback_info info = {0};

	if ((info.diff = git_diff_list_alloc(repo, opts)) == NULL)
		return GIT_ENOMEM;

	if ((error = git_repository_index(&info.index, repo)) == GIT_SUCCESS) {
		error = diff_workdir_walk(NULL, &info, diff_workdir_to_index_cb);
		if (error == GIT_SUCCESS)
			error = add_new_index_deltas(&info, GIT_STATUS_DELETED, NULL);
		git_index_free(info.index);
	}
	git_buf_free(&info.diff->pfx);

	if (error != GIT_SUCCESS)
		git_diff_list_free(info.diff);
	else
		*diff = info.diff;

	return error;
}

} diff_output_info;
	diff_output_info *info = priv;
	if (len == 1 && info->hunk_cb) {
				err = info->hunk_cb(
					info->cb_data, info->delta, &range, bufs[0].ptr, bufs[0].size);
	else if ((len == 2 || len == 3) && info->line_cb) {
		err = info->line_cb(
			info->cb_data, info->delta, origin, bufs[1].ptr, bufs[1].size);
			err = info->line_cb(
				info->cb_data, info->delta, origin, bufs[2].ptr, bufs[2].size);
	diff_output_info info;
	info.diff    = diff;
	info.cb_data = data;
	info.hunk_cb = hunk_cb;
	info.line_cb = line_cb;
	xdiff_callback.priv = &info;
	git_vector_foreach(&diff->files, info.index, delta) {
			error = file_cb(data, delta, (float)info.index / diff->files.length);
		info.delta = delta;
} diff_print_info;
	diff_print_info *pi = data;
	diff_print_info pi;
static int print_oid_range(diff_print_info *pi, git_diff_delta *delta)
	diff_print_info *pi = data;
	diff_print_info *pi = data;
	diff_print_info *pi = data;
	diff_print_info pi;
	diff_output_info info;
	info.diff    = NULL;
	info.delta   = &delta;
	info.cb_data = cb_data;
	info.hunk_cb = hunk_cb;
	info.line_cb = line_cb;
	xdiff_callback.priv = &info;