import os
from bs4 import BeautifulSoup
from difflib import unified_diff
from tqdm import tqdm
from rich.console import Console
from rich.table import Table
from rich import box

IGNORED_TAGS = {'version'}  # Add more tags to ignore as needed

def load_and_normalize(path):
    """Load, normalize, and return XML content as a list of lines."""
    with open(path, 'r', encoding='utf-8') as f:
        soup = BeautifulSoup(f, 'xml')

    for parent in soup.find_all(recursive=False):
        for container in parent.find_all(recursive=False):
            children = container.find_all(recursive=False)
            if all(child.find('id') for child in children):
                sorted_children = sorted(children, key=lambda x: x.find('id').text.strip())
                [child.extract() for child in children]
                for child in sorted_children:
                    container.append(child)

    # Ignore dynamic or acceptable differences
    for tag in IGNORED_TAGS:
        for match in soup.find_all(tag):
            match.string = ''

    return soup.prettify().splitlines(keepends=True)

def compare_files(original_dir, new_dir):
    console = Console()
    orig_files = {f for f in os.listdir(original_dir) if f.endswith('.xml')}
    new_files = {f for f in os.listdir(new_dir) if f.endswith('.xml')}
    all_files = sorted(orig_files | new_files)

    results = []

    for filename in tqdm(all_files, desc="Comparing files"):
        orig_path = os.path.join(original_dir, filename)
        new_path = os.path.join(new_dir, filename)

        if not os.path.exists(orig_path):
            results.append((filename, "[red]MISSING in Original[/red]", []))
            continue
        if not os.path.exists(new_path):
            results.append((filename, "[red]MISSING in New[/red]", []))
            continue

        orig_lines = load_and_normalize(orig_path)
        new_lines = load_and_normalize(new_path)

        if orig_lines == new_lines:
            results.append((filename, "[green]IDENTICAL[/green]", []))
        elif sorted(orig_lines) == sorted(new_lines):
            results.append((filename, "[yellow]REORDERED ONLY[/yellow]", []))
        else:
            diff = list(unified_diff(orig_lines, new_lines, fromfile='original', tofile='new'))
            results.append((filename, "[red]DIFFERENT[/red]", diff))

    # Display result summary table
    table = Table(title="XML Comparison Results", box=box.SIMPLE_HEAVY)
    table.add_column("File", style="bold")
    table.add_column("Status", style="bold")

    for fname, status, _ in results:
        table.add_row(fname, status)

    console.print(table)

    # Optionally show diffs
    for fname, status, diff in results:
        if diff:
            console.rule(f"[bold red]Diff for {fname}")
            console.print("".join(diff), highlight=True)

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Compare XML files in two directories.")
    parser.add_argument("original", help="Directory with original XML files")
    parser.add_argument("new", help="Directory with new XML files")
    args = parser.parse_args()

    compare_files(args.original, args.new)