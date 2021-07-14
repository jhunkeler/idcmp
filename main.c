#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <pwd.h>
#include <limits.h>
#include <unistd.h>

#if defined(__APPLE__)
    // Force Darwin to emit more than 32 damn groups
    #undef NGROUPS_MAX
    #define NGROUPS_MAX 0x10000
#endif

struct Groups {
    gid_t *gids;
    int ngids;
    struct group **group;
};

struct User {
    struct passwd *pw;
    struct Groups *gr;
};

struct User *user_init() {
    struct User *result;

    result = malloc(sizeof(*result));
    result->pw = malloc(sizeof(*result->pw));
    result->gr = malloc(sizeof(*result->gr));
    result->gr->ngids = NGROUPS_MAX;
    result->gr->gids = malloc(NGROUPS_MAX * sizeof(*result->gr->gids));
    result->gr->group = calloc(result->gr->ngids, sizeof(**result->gr->group));
    return result;
}

struct User *get_account_info(char *user) {
    struct passwd *pw;
    struct group *gr;
    struct User *result;

    if ((pw = getpwnam(user)) == NULL) {
        return NULL;
    }

    // getpwnam returns static storage, so copy contents.
    result = user_init();
    result->pw->pw_name = strdup(pw->pw_name);
    result->pw->pw_gid = pw->pw_gid;
    result->pw->pw_dir = strdup(pw->pw_dir);
    result->pw->pw_gecos = strdup(pw->pw_gecos);
    result->pw->pw_shell = strdup(pw->pw_shell);
    result->pw->pw_uid = pw->pw_uid;
    result->pw->pw_passwd = strdup(pw->pw_passwd);

    // populate gid list
    if (getgrouplist(result->pw->pw_name, result->pw->pw_gid, result->gr->gids, &result->gr->ngids) == -1) {
        perror("oh no");
        exit(EXIT_FAILURE);
    }

    // getgrgid returns static storage, so copy contents.
    for (size_t i = 0; i < result->gr->ngids; i++) {
        result->gr->group[i] = calloc(1, sizeof(*result->gr->group[i]));
        gr = getgrgid(result->gr->gids[i]);
        if (!gr) {
            fprintf(stderr, "Failed to read group information for GID: %d\n", result->gr->gids[i]);
            result->gr->group[i]->gr_name = strdup("(no name)");
            result->gr->group[i]->gr_gid = result->gr->gids[i];
            result->gr->group[i]->gr_mem = NULL;
            result->gr->group[i]->gr_passwd = strdup("");
            continue;
        }
        result->gr->group[i]->gr_name = strdup(gr->gr_name);
        result->gr->group[i]->gr_gid = gr->gr_gid;

        size_t j = 0;
        for (j = 0; gr->gr_mem[j] != NULL; j++);
        result->gr->group[i]->gr_mem = calloc(j + 1, sizeof(*result->gr->group[i]->gr_mem));

        for (j = 0; gr->gr_mem[j] != NULL; j++) {
            result->gr->group[i]->gr_mem[j] = strdup(gr->gr_mem[j]);
        }

        result->gr->group[i]->gr_passwd = strdup(gr->gr_passwd);
    }

    return result;
}

void free_account_info(struct User *user) {
    free(user->pw->pw_name);
    free(user->pw->pw_shell);
    free(user->pw->pw_gecos);
    free(user->pw->pw_dir);
    free(user->pw->pw_passwd);
    free(user->pw);

    for (size_t i = 0; i < user->gr->ngids; i++) {
        for (size_t j = 0; user->gr->group[i]->gr_mem != NULL && user->gr->group[i]->gr_mem[j] != NULL; j++) {
            free(user->gr->group[i]->gr_mem[j]);
        }
        free(user->gr->group[i]->gr_mem);
        free(user->gr->group[i]->gr_passwd);
        free(user->gr->group[i]->gr_name);
        free(user->gr->group[i]);
    }
    free(user->gr->group);
    free(user->gr->gids);
    free(user->gr);
    free(user);
}

int has_group(struct User *user, char *name) {
    int found;

    found = 0;
    for (size_t i = 0; i < user->gr->ngids; i++) {
        if (user->gr->group[i] == NULL) {
            continue;
        }
        if (strcmp(user->gr->group[i]->gr_name, name) == 0) {
            found = 1;
            break;
        }
    }
    return found;
}

int group_compare(struct User *a, struct User *b, char *name) {
    int b_contains_a, a_contains_b;

    a_contains_b = has_group(b, name);
    b_contains_a = has_group(a, name);

    if(a_contains_b < b_contains_a) {
        return -1;
    } else if (a_contains_b > b_contains_a) {
        return 1;
    }
    return 0;
}

static int group_sort_name(const void *a, const void *b) {
    struct group *aa = (*(struct group **) a);
    struct group *bb = (*(struct group **) b);

    if (strcmp(aa->gr_name, bb->gr_name) < 0) {
        return -1;
    } else if (strcmp(aa->gr_name, bb->gr_name) > 0) {
        return 1;
    }
    return 0;
}

static int group_sort_id(const void *a, const void *b) {
    struct group *aa = (*(struct group **) a);
    struct group *bb = (*(struct group **) b);

    if (aa->gr_gid < bb->gr_gid) {
        return -1;
    } else if (aa->gr_gid > bb->gr_gid) {
        return 1;
    }
    return 0;
}

/**
 * Return the length of the longest string in an array
 */
size_t strmax(char **arr) {
    size_t result;

    result = 0;
    for (int i = 1; arr[i] != NULL; i++) {
        if (strlen(arr[i]) > strlen(arr[result])) {
            result = i;
        }
    }
    return strlen(arr[result]);
}


typedef int (*compar)(const void *, const void *);

int main(int argc, char *argv[]) {
    struct User *user_a, *user_b;
    int rec;
    int ngroup_all;
    struct group **group_all;
    char **group_names;
    compar fn_sort = &group_sort_id;

    if (argc < 3) {
        fprintf(stderr, "usage: %s [-n] {user_a} {user_b}\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int arg;
    size_t positional;
    char *users[2];
    for (arg = 1, positional = 0; arg < argc; arg++) {
        if (strcmp(argv[arg], "-n") == 0) {
            fn_sort = &group_sort_name;
            continue;
        }
        if (positional > 1) {
            break;
        }
        users[positional] = argv[arg];
        positional++;
    }

    if ((user_a = get_account_info(users[0])) == NULL) {
        fprintf(stderr, "Invalid user: '%s'\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    if ((user_b = get_account_info(users[1])) == NULL) {
        fprintf(stderr, "Invalid user: '%s'\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    ngroup_all = user_a->gr->ngids + user_b->gr->ngids;
    group_all = calloc(ngroup_all, sizeof(struct group *));
    group_names = calloc(ngroup_all, sizeof(char *));

    // Merge all group into a single list
    rec = 0;
    for (rec = 0; rec < user_a->gr->ngids; rec++) {
        group_all[rec] = user_a->gr->group[rec];
    }
    for (int i = 0; i < user_b->gr->ngids; i++) {
        if (has_group(user_a, user_b->gr->group[i]->gr_name)) {
            continue;
        }
        group_all[rec] = user_b->gr->group[i];
        rec++;
    }

    qsort(group_all, rec, sizeof(struct group *), fn_sort);

    // Extract all group names from both users
    for (size_t i = 0; group_all[i] != NULL; i++) {
        group_names[i] = group_all[i]->gr_name;
    }

    // Construct dynamic width output header
    int maxfmt = 255;
    char *fmthdr = malloc(maxfmt);
    char *fmt = malloc(maxfmt);
    char *hdr[] = {
            "GID NAME", "GID", "COMMON TO", NULL
    };
    size_t longest_group = strmax(group_names);

    // Generate column titles
    sprintf(fmthdr, "%%-%zus   %%-5s   %%s\n", longest_group);
    printf(fmthdr, hdr[0], hdr[1], hdr[2]);

    // Display records
    sprintf(fmt, "%%-%zus | %%-5d | ", longest_group);
    for (size_t i = 0; i < rec; i++) {
        int contains;

        printf(fmt, group_all[i]->gr_name, group_all[i]->gr_gid);

        contains = group_compare(user_a, user_b, group_all[i]->gr_name);
        if (contains < 0) {
            puts(user_a->pw->pw_name);
        } else if (contains > 0){
            puts(user_b->pw->pw_name);
        } else {
            puts("both");
        }
    }

    free(fmthdr);
    free(fmt);
    free_account_info(user_a);
    free_account_info(user_b);
    free(group_names);
    for (size_t i = 0; i < ngroup_all; i++) {
        group_all[i] = NULL;
        free(group_all[i]);
    }
    free(group_all);
    return 0;
}
