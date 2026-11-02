# uia_dump.ps1 - prints a window's raw UI Automation tree (control type, name, rect,
# OFF/DIS/FOC flags, patterns, LabeledBy) without bringing it to the front, through
# the managed UIAutomationClient. Find the window by -TitlePart or -ProcId; -All dumps
# everything, -Match prints the chain, siblings and subtree around elements whose
# name holds the text. Korean text goes in as base64 of UTF-16LE (-TitleB64, -MatchB64):
#   [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("...")) 
# Example: powershell -File tools/uia_dump.ps1 -TitlePart "label mock" -All
param([string]$TitlePart = "", [int]$ProcId = 0, [string]$Match = "", [int]$MaxDepth = 60, [switch]$All, [string]$TitleB64 = "", [string]$MatchB64 = "")
if ($TitleB64 -ne "") { $TitlePart = [System.Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($TitleB64)) }
if ($MatchB64 -ne "") { $Match = [System.Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($MatchB64)) }
$script:out = New-Object System.Collections.Generic.List[string]
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$root = [System.Windows.Automation.AutomationElement]::RootElement
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)
$target = $null
foreach ($w in $wins) {
  try {
    $p = $w.Current.ProcessId; $n = $w.Current.Name; $c = $w.Current.ClassName
    if (($ProcId -ne 0 -and $p -eq $ProcId -and $c -like "Chrome_WidgetWin*") -or ($TitlePart -ne "" -and $n -like "*$TitlePart*")) { $target = $w; break }
  } catch {}
}
if (-not $target) {
  Write-Output "no window; top-level windows:"
  foreach ($w in $wins) { try { Write-Output ("  pid " + $w.Current.ProcessId + " " + $w.Current.ClassName + " '" + $w.Current.Name + "'") } catch {} }
  exit 1
}
$script:out.Add("window: '" + $target.Current.Name + "' pid " + $target.Current.ProcessId + " rect " + $target.Current.BoundingRectangle)
$walker = [System.Windows.Automation.TreeWalker]::RawViewWalker
$pats = @(
  @{n="Invoke";id=[System.Windows.Automation.InvokePattern]::Pattern},
  @{n="Toggle";id=[System.Windows.Automation.TogglePattern]::Pattern},
  @{n="SelItem";id=[System.Windows.Automation.SelectionItemPattern]::Pattern},
  @{n="Expand";id=[System.Windows.Automation.ExpandCollapsePattern]::Pattern},
  @{n="Scroll";id=[System.Windows.Automation.ScrollPattern]::Pattern})
function Line($el, $depth) {
  try {
    $cur = $el.Current
    $ct = $cur.ControlType.ProgrammaticName -replace '^ControlType\.',''
    $r = $cur.BoundingRectangle
    $rs = if ($r.IsEmpty) { "empty" } else { "{0},{1} {2}x{3}" -f [int]$r.X,[int]$r.Y,[int]$r.Width,[int]$r.Height }
    $flags = @()
    if ($cur.IsOffscreen) { $flags += "OFF" }
    if (-not $cur.IsEnabled) { $flags += "DIS" }
    if ($cur.IsKeyboardFocusable) { $flags += "FOC" }
    if (-not $cur.IsControlElement) { $flags += "noctl" }
    foreach ($pt in $pats) { try { $v = $el.GetCurrentPropertyValue($pt.id) ; if ($v) { $flags += $pt.n } } catch {} }
    $lb = ""
    try { $l = $cur.LabeledBy; if ($l) { $lr = $l.Current.BoundingRectangle; $lb = " labeledby=[" + ($l.Current.ControlType.ProgrammaticName -replace '^ControlType\.','') + " '" + $l.Current.Name + "' " + ("{0},{1} {2}x{3}" -f [int]$lr.X,[int]$lr.Y,[int]$lr.Width,[int]$lr.Height) + "]" } } catch { $lb = " labeledby=ERR" }
    $name = $cur.Name; if ($name.Length -gt 60) { $name = $name.Substring(0,60) }
    $lct = $cur.LocalizedControlType
    return (("  " * $depth) + $ct + "/" + $lct + " '" + $name + "' " + $rs + " [" + ($flags -join ",") + "]" + $lb + " aid=" + $cur.AutomationId)
  } catch { return (("  " * $depth) + "ERR " + $_.Exception.Message) }
}
function Children($el) {
  $out = @()
  $child = $null
  try { $child = $walker.GetFirstChild($el) } catch {}
  while ($child) { $out += $child; try { $child = $walker.GetNextSibling($child) } catch { $child = $null } }
  return ,$out
}
function DumpAll($el, $depth) {
  if ($depth -gt $MaxDepth) { return }
  $script:out.Add((Line $el $depth))
  foreach ($c in (Children $el)) { DumpAll $c ($depth + 1) }
}
# Returns $true when the subtree holds a match; records the chain and siblings.
function Walk($el, $depth) {
  if ($depth -gt $MaxDepth) { return $false }
  $isMatch = $false
  try { $isMatch = ($Match -ne "" -and $el.Current.Name -like "*$Match*") } catch {}
  if ($isMatch) { $script:out.Add("MATCH at depth " + $depth); DumpAll $el $depth; return $true }
  $kids = Children $el
  $hit = @()
  $any = $false
  foreach ($c in $kids) { $r = Walk $c ($depth + 1); $hit += [bool]$r; if ($r) { $any = $true } }
  if ($any) {
    $script:out.Add((Line $el $depth))
    for ($i = 0; $i -lt $kids.Count; $i++) { if (-not $hit[$i]) { $script:out.Add((Line $kids[$i] ($depth + 1)) + "  (sibling)") } }
  }
  return $any
}
if ($All) { DumpAll $target 0 } else { [void](Walk $target 0) }
$script:out
