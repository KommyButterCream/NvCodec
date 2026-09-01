param([string]$Root)

# NvCodec DLL 은 STL 을 쓰지 않는다.
#
# 금지 대상은 컨테이너 / 문자열 / 알고리즘 / iostream 처럼 CRT 와 예외 기계장치를
# 끌고 오는 헤더다. <new>, <stdint.h>, <cstdint> 는 freestanding 서브셋이라 허용한다.
# new 는 over-aligned 타입(alignof 64)을 자동으로 정렬해 주므로 malloc 계열로
# 대체하면 오히려 UB 가 된다.
$banned = @(
  'vector','string','string_view','map','unordered_map','set','unordered_set',
  'list','deque','array','memory','algorithm','functional','optional','variant',
  'iostream','sstream','fstream','ostream','istream','thread','mutex',
  'condition_variable','future','chrono','filesystem','regex','tuple','atomic'
)
$pattern = '#include\s*<(' + ($banned -join '|') + ')>'

$dirs = @('NvEncode','NvDecode','NvCodec') | ForEach-Object { Join-Path $Root $_ }
$hits = Get-ChildItem -Path $dirs -Include *.h,*.cpp -Recurse -ErrorAction SilentlyContinue |
        Select-String -Pattern $pattern

if ($hits) {
  foreach ($h in $hits) {
    Write-Host "$($h.Path)($($h.LineNumber)): error : STL header is not allowed in the NvCodec DLL -- $($h.Line.Trim())"
  }
  exit 1
}
Write-Host "no-STL check passed"
exit 0
