"""Behavioral fixtures for the UI source/evidence gate; no Windows dependency."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('check_ui', ROOT / 'tools/check_ui.py')
ui = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ui
spec.loader.exec_module(ui)


class Contracts(unittest.TestCase):
    def rules(self, source):
        return {f.rule for f in ui.scan('src/platform_win/example.cpp', source)}

    def test_shared_and_native_input_are_allowed(self):
        self.assertFalse(self.rules('''
            auto list=fluent::list_box(surface,parent,name,1);
            auto edit=child(parent,L"EDIT",L"2×3 ↑",ES_AUTOHSCROLL,2);
            const auto paper=0xe7e7e7; const auto green=0x00e70d; const auto mask=0xffff;
            set_ui_text(edit,L"LISTBOX"); auto log=R"(DrawFocusRect(hdc,&r);)";
            // CreateWindowExW(0,TOOLTIPS_CLASSW,L"",0);
            /* const auto glyph=L'×'; DrawFocusRect(hdc,&r); */
            surface.draw_card(h,dc,r,{CardPart::icon_button,Icon::close});
        '''))

    def test_known_escape_hatches_fail(self):
        self.assertEqual(self.rules('''
            auto a=child(parent,L"LISTBOX",L"",WS_VISIBLE,1);
            auto b=CreateWindowExW(0,WC_COMBOBOXW,L"",WS_VISIBLE);
            auto c=create_ui_window(0,TOOLTIPS_CLASSW,L"",0);
            DrawFocusRect(dc,&r); MessageBoxW(h,L"Oops",L"Error",0);
            auto glyph=0xe70d; draw_text(dc,r,L"⌄",font,color);
            find_button(1,L'↑'); ink.glyph=L'×';
        '''), {'native-selection','native-tooltip','gdi-focus','system-dialog','icon-codepoint','text-icon'})

    def test_escaped_and_lowercase_literals(self):
        self.assertIn('native-selection', self.rules('CreateWindowW(L"combobox",L"",0);'))
        self.assertIn('icon-codepoint', self.rules(r"auto icon=L'\ue70d';"))
        self.assertIn('icon-codepoint', self.rules(r"auto icon=L'\U0000e70d';"))
        self.assertIn('text-icon', self.rules(r"find_button(1,L'\u2191');"))
        self.assertIn('native-tooltip',self.rules('auto type=TOOLTIPS_CLASSW; CreateWindowExW(0,type,L"",0);'))
        self.assertIn('native-selection',self.rules('auto type=L"LISTBOX"; child(h,type,L"",0,1);'))
        self.assertIn('native-selection',self.rules('child(h,LR"(LISTBOX)",L"",0,1);'))

    def test_precise_exception_and_stale_detection(self):
        path, code = 'src/platform_win/adapter.cpp', 'child(h,L"LISTBOX",L"",0,1);'
        findings = ui.scan(path, code)
        exception = dict(rule='native-selection', path=path, anchor='L"LISTBOX"',
                         reason='Shared adapter supplies native semantics', review_when='Creation path changes')
        self.assertEqual(ui.apply_exceptions(findings,{path:code},[exception]), [])
        self.assertTrue(ui.apply_exceptions(findings,{path:code},[exception,exception]))
        self.assertTrue(ui.apply_exceptions([],{path:'fluent::list_box();'},[exception]))
        self.assertTrue(ui.apply_exceptions(findings,{path:code},[{**exception,'path':'src/*'}]))

    def test_comment_does_not_move_exception(self):
        path, code = 'src/a.cpp', '// child(h,L"LISTBOX");\nchild(h,L"LISTBOX");'
        self.assertEqual(ui.scan(path,code)[0].line,2)

    def test_resource_controls_are_not_an_escape_hatch(self):
        self.assertEqual({f.rule for f in ui.scan('resources/dialog.rc','''
            LISTBOX IDC_LIST, 0,0,100,100,LBS_NOTIFY
            CONTROL "Names",IDC_NAMES,"ComboBox",CBS_DROPDOWNLIST,0,0,100,100
            CONTROL "",IDC_TIP,"tooltips_class32",0,0,0,100,100
        ''')},{'native-selection','native-tooltip'})
        self.assertFalse(ui.scan('resources/dialog.rc','CONTROL "LISTBOX",IDC_LABEL,"Static",0,0,0,100,20'))
        self.assertTrue(ui.ui_path('resources/dialog.rc'))

    def test_evidence_is_bound_to_source_and_review(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); source='src/platform_win/widget.cpp'
            (root / source).parent.mkdir(parents=True); (root / source).write_text('new UI')
            name='tests/compatibility/fluent/evidence/widget.json'
            (root / name).parent.mkdir(parents=True)
            (root / 'review.md').write_text('Reviewed states; physical checks remain open.')
            (root / 'capture.png').write_bytes(b'\x89PNG\r\n\x1a\n'+b'capture fixture ' * 8)
            self.assertEqual(ui.source_hash(root/'capture.png'),ui.hashlib.sha256((root/'capture.png').read_bytes()).hexdigest())
            data=dict(status='accepted_with_open_checks', reviewer='Fixture', conclusion='States inspected',
                      review='review.md', checks=['UI-17.components'], open_checks=['Physical DPI'],
                      sources={source:ui.source_hash(root/source)}, screenshots={'before':['capture.png'],'after':['capture.png']},
                      checklist={key:dict(status='passed',evidence='Synthetic fixture') for key in ui.UI_CHECKS})
            (root/name).write_text(json.dumps(data))
            self.assertEqual(ui.check_evidence(root,{source,name}),[])
            data['checklist']['physical']['evidence']=''
            (root/name).write_text(json.dumps(data))
            self.assertTrue(ui.check_evidence(root,{source,name}))
            data['checklist']['physical']=dict(status='open',evidence='Physical DPI requires a second monitor')
            (root/name).write_text(json.dumps(data))
            self.assertEqual(ui.check_evidence(root,{source,name}),[])
            self.assertTrue(ui.check_evidence(root,{source}))
            (root/source).write_text('later UI')
            self.assertTrue(ui.check_evidence(root,{source,name}))
            data['status']='captured'; (root/name).write_text(json.dumps(data))
            self.assertTrue(ui.check_evidence(root,{source,name}))
            (root/source).unlink()
            self.assertTrue(ui.check_evidence(root,{source}))
            data['status']='accepted_with_open_checks'; data['sources'][source]='deleted'
            (root/name).write_text(json.dumps(data))
            self.assertEqual(ui.check_evidence(root,{source,name}),[])


if __name__ == '__main__':
    unittest.main()
