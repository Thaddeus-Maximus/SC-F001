#!/usr/bin/env python3
"""
Enhanced HTML/CSS/JS Minifier and Compressor
This script provides multiple levels of optimization for embedded web pages
"""

import minify_html
import gzip
import brotli
import re
import sys
from pathlib import Path

# Try to import optional JavaScript minifier
try:
    import jsmin
    HAS_JSMIN = True
except ImportError:
    HAS_JSMIN = False
    print("Warning: jsmin not installed. Install with: pip install jsmin --break-system-packages")

# Try to import rjsmin (faster alternative)
try:
    import rjsmin
    HAS_RJSMIN = True
except ImportError:
    HAS_RJSMIN = False


def extract_and_minify_js(html_content):
    """Extract JavaScript, minify it, and reinsert into HTML"""
    if not (HAS_JSMIN or HAS_RJSMIN):
        return html_content
    
    # Find all script tags
    script_pattern = re.compile(r'<script>(.*?)</script>', re.DOTALL)
    
    def minify_script_content(match):
        script_content = match.group(1)
        
        # Skip if empty or too small
        if len(script_content.strip()) < 50:
            return match.group(0)
        
        try:
            # Use rjsmin if available (faster), otherwise jsmin
            if HAS_RJSMIN:
                minified = rjsmin.jsmin(script_content)
            elif HAS_JSMIN:
                minified = jsmin.jsmin(script_content)
            else:
                minified = script_content
            
            return f'<script>{minified}</script>'
        except Exception as e:
            print(f"Warning: JS minification failed: {e}")
            return match.group(0)
    
    return script_pattern.sub(minify_script_content, html_content)


def aggressive_html_minify(html_content):
    """Apply aggressive minification strategies"""
    
    # First pass: minify JavaScript
    html_content = extract_and_minify_js(html_content)
    
    # Use minify_html library
    minified = minify_html.minify(
        html_content,
        minify_js=True,
        minify_css=True,
        keep_comments=False,
        keep_html_and_head_opening_tags=False,
        keep_closing_tags=True,  # Safer - can set to False for more aggressive
        remove_processing_instructions=True,
        remove_bangs=False,  # Keep <!DOCTYPE>
        #do_not_minify_doctype=True,
        #ensure_spec_compliant_unquoted_attribute_values=True,
        #keep_spaces_between_attributes=False,
    )
    
    return minified


def generate_c_header(data, variable_name="html_content", use_progmem=True):
    """Generate C/C++ header file with compressed data"""
    lines = []
    
    # Add header guard
    guard = variable_name.upper() + "_H"
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    
    # Add includes if using PROGMEM
    if use_progmem:
        lines.append("#include <Arduino.h>")
        lines.append("")
    
    # Add array declaration
    progmem_keyword = "PROGMEM " if use_progmem else ""
    lines.append(f"const unsigned char {progmem_keyword}{variable_name}[] = {{")
    
    # Format bytes in rows of 16
    hex_bytes = [f'0x{byte:02x}' for byte in data]
    for i in range(0, len(hex_bytes), 16):
        row = hex_bytes[i:i+16]
        lines.append("    " + ", ".join(row) + ",")
    
    lines.append("};")
    lines.append("")
    lines.append(f"const unsigned int {variable_name}_len = {len(data)};")
    lines.append("")
    lines.append(f"#endif // {guard}")
    
    return "\n".join(lines)


def print_compression_stats(original_size, minified_size, gzip_size, brotli_size=None):
    """Print compression statistics"""
    print("\n" + "="*60)
    print("COMPRESSION STATISTICS")
    print("="*60)
    print(f"Original HTML:        {original_size:,} bytes")
    print(f"Minified HTML:        {minified_size:,} bytes ({minified_size/original_size*100:.1f}%)")
    print(f"Gzip (level 9):       {gzip_size:,} bytes ({gzip_size/original_size*100:.1f}%)")
    
    if brotli_size:
        print(f"Brotli (level 11):    {brotli_size:,} bytes ({brotli_size/original_size*100:.1f}%)")
    
    print(f"\nSavings (gzip):       {original_size - gzip_size:,} bytes ({(1-gzip_size/original_size)*100:.1f}% reduction)")
    
    if brotli_size:
        print(f"Savings (brotli):     {original_size - brotli_size:,} bytes ({(1-brotli_size/original_size)*100:.1f}% reduction)")
    
    print("="*60 + "\n")


def main():
    input_file = "webpage.html"
    
    # Check if input file exists
    if not Path(input_file).exists():
        print(f"Error: {input_file} not found")
        sys.exit(1)
    
    # Read original HTML
    print(f"Reading {input_file}...")
    with open(input_file, "r", encoding="utf-8") as fin:
        original_html = fin.read()
    
    original_size = len(original_html.encode('utf-8'))
    
    # Minify HTML
    print("Minifying HTML/CSS/JS...")
    minified_html = aggressive_html_minify(original_html)
    minified_size = len(minified_html.encode('utf-8'))
    
    # Save minified HTML
    with open("webpage_minified.html", "w", encoding="utf-8") as fout:
        fout.write(minified_html)
    print("Saved: webpage_minified.html")
    
    # Compress with gzip
    print("Compressing with gzip (level 9)...")
    minified_bytes = minified_html.encode('utf-8')
    gzipped_bytes = gzip.compress(minified_bytes, compresslevel=9)
    gzip_size = len(gzipped_bytes)
    
    # Compress with brotli (if available)
    brotli_size = None
    brotli_bytes = None
    try:
        print("Compressing with brotli (level 11)...")
        brotli_bytes = brotli.compress(minified_bytes, quality=11)
        brotli_size = len(brotli_bytes)
    except Exception as e:
        print(f"Brotli compression not available: {e}")
    
    # Generate C headers
    print("\nGenerating C header files...")
    
    # Gzip version
    with open("webpage_gzip.h", "w") as fout:
        fout.write(generate_c_header(gzipped_bytes, "html_content_gz", use_progmem=True))
    print("Saved: webpage_gzip.h")
    
    # Brotli version (if available)
    if brotli_bytes:
        with open("webpage_brotli.h", "w") as fout:
            fout.write(generate_c_header(brotli_bytes, "html_content_br", use_progmem=True))
        print("Saved: webpage_brotli.h")
    
    # Also generate the old format for compatibility
    with open("webpage.h", "w") as fout:
        fout.write("const char html_content[] = {")
        fout.write(','.join(f'0x{byte:02x}' for byte in gzipped_bytes))
        fout.write("};\n\n")
        fout.write(f"const unsigned int html_content_len = {len(gzipped_bytes)};\n")
    print("Saved: webpage.h (legacy format)")
    
    # Print statistics
    print_compression_stats(original_size, minified_size, gzip_size, brotli_size)
    
    # Recommendations
    print("RECOMMENDATIONS:")
    print("-" * 60)
    if brotli_size and brotli_size < gzip_size:
        savings = gzip_size - brotli_size
        print(f"Use Brotli compression (saves {savings} bytes vs gzip)")
        print(f"  Include: webpage_brotli.h")
    else:
        print(f"Use Gzip compression")
        print(f"  Include: webpage_gzip.h")
    
    if not HAS_RJSMIN and not HAS_JSMIN:
        print("\nInstall rjsmin for better JS compression:")
        print("  pip install rjsmin --break-system-packages")
    
    print("-" * 60)


if __name__ == "__main__":
    main()