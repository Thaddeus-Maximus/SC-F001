import minify_html
import gzip

with open("landingpage.html", "r", encoding="utf-8") as fin:
    original_html = fin.read()

minified_html = minify_html.minify(
    original_html,
    minify_js=True,
    minify_css=True,
    keep_comments=False,                    # Remove comments
    keep_html_and_head_opening_tags=False,  # Remove <html> and <head> if possible
    keep_closing_tags=True,                 # Keep closing tags (safer; set False for more aggression if you test thoroughly)
    remove_processing_instructions=True,    # Remove <?xml ...> etc.
    #keep_comments=False,
    remove_bangs=False                      # Keep <!DOCTYPE> bang (removing saves bytes but can break some edge cases)
)

with open("webpage_minified.html", "w") as fout:
	fout.write(minified_html)
	
	
minified_bytes = minified_html.encode('utf-8')
gzipped_bytes = gzip.compress(minified_bytes, compresslevel=9)
	

with open("webpage.h", "w") as fout:
    fout.write("const char html_content[] = {")
    fout.write(','.join(f'0x{byte:02x}' for byte in gzipped_bytes))
    fout.write("};\n\n")
    fout.write(f"const unsigned int html_content_len = {len(gzipped_bytes)};\n")