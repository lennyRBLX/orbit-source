#include "pspkg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    char page[256], platform[4], game_id[13];
    const char *game = "https://pspkg.com/ps5/grand-theft-auto-vi-14552.html";
    assert(pspkg_download_page(game, page, sizeof page));
    assert(!strcmp(page, "https://pspkg.com/ps5/download-grand-theft-auto-vi-14552.html"));
    assert(pspkg_page_identity(page, platform, sizeof platform, game_id, sizeof game_id));
    assert(!strcmp(platform, "ps5") && !strcmp(game_id, "14552"));
    assert(pspkg_page_redirect(page, "https://pspkg.com/ar/ps5/download-grand-theft-auto-vi-14552.html?slug=grand-theft-auto-vi&id=14552"));
    assert(pspkg_page_redirect(page, "https://pspkg.com/ps5/download-grand-theft-auto-vi-14552.html?id=14552&id=14552"));
    assert(!pspkg_page_redirect(page, "https://pspkg.com/ar/ps5/download-grand-theft-auto-vi-14553.html"));
    assert(!pspkg_page_redirect(page, "https://example.com/ar/ps5/download-grand-theft-auto-vi-14552.html"));
    const char *route = "https://pspkg.com/ps5/download.php?game_id=14552&edition_id=29336&link_id=85873";
    assert(pspkg_link_matches(route, page, platform, game_id, "29336"));
    assert(!pspkg_link_matches(route, page, platform, "14553", "29336"));
    assert(!pspkg_link_matches(route, page, platform, game_id, "29337"));
    assert(!pspkg_download_page("https://pspkg.com/ps5-arabic/grand-theft-auto-vi-14552.html", page, sizeof page));
    assert(!pspkg_page_identity("https://pspkg.com/ps5/download-grand-theft-auto-vi-14552.html?id=7",
                                platform, sizeof platform, game_id, sizeof game_id));
    assert(pspkg_download_page("https://pspkg.com/ps4/dark-light-14441.html", page, sizeof page));
    assert(pspkg_page_identity(page, platform, sizeof platform, game_id, sizeof game_id));
    assert(!strcmp(platform, "ps4") && !strcmp(game_id, "14441"));

    /* PSPKG's successful reveal response contains relative links in HTML,
     * sometimes with HTML-escaped query separators. Reject other game IDs. */
    const char *html = "<a href=\"download.php?game_id=14552&edition_id=29336&link_id=85873&amp;route=browser\">Viki</a>"
                       "<a href=\"download.php?game_id=14552&amp;edition_id=29336&amp;link_id=85874\">File k</a>"
                       "<a href=\"download.php?game_id=14553&edition_id=29336&link_id=85875\">Other</a>";
    PspkgLink links[PSPKG_LINK_LIMIT] = {{0}};
    size_t count = 0;
    pspkg_scan_links((const unsigned char *)html, strlen(html), "ps5", "14552", links, &count, PSPKG_LINK_LIMIT);
    assert(count == 2);
    assert(!strcmp(links[0].url, "https://pspkg.com/ps5/download.php?game_id=14552&edition_id=29336&link_id=85873&route=browser"));
    assert(!strcmp(links[1].url, "https://pspkg.com/ps5/download.php?game_id=14552&edition_id=29336&link_id=85874"));
    pspkg_scan_links((const unsigned char *)html, strlen(html), "ps5", "14552", links, &count, PSPKG_LINK_LIMIT);
    assert(count == 2);
    unsigned char wide[512] = {0};
    const char *link = "download.php?game_id=14441&edition_id=28868&link_id=83940";
    for (size_t i = 0; i < strlen(link); i++) wide[2 * i] = (unsigned char)link[i];
    count = 0;
    pspkg_scan_links(wide, 2 * strlen(link), "ps4", "14441", links, &count, PSPKG_LINK_LIMIT);
    assert(count == 1);
    assert(!strcmp(links[0].link_id, "83940"));
    if (argc == 2) {
        FILE *fixture = fopen(argv[1], "rb");
        assert(fixture);
        assert(!fseek(fixture, 0, SEEK_END));
        long length = ftell(fixture);
        assert(length > 0 && length < 1024 * 1024);
        assert(!fseek(fixture, 0, SEEK_SET));
        unsigned char *body = malloc((size_t)length);
        assert(body && fread(body, 1, (size_t)length, fixture) == (size_t)length);
        fclose(fixture);
        count = 0;
        pspkg_scan_links(body, (size_t)length, "ps5", "14552", links, &count, PSPKG_LINK_LIMIT);
        free(body);
        assert(count == 12);
        printf("Captured PSPKG response: %zu links\n", count);
    }
    puts("PSPKG parser checks passed");
}
